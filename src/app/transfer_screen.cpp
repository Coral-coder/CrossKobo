// Wireless transfer: the address to open in a browser on the same network,
// the key that gates it, and what has arrived. The server itself runs on
// its own thread; this screen only starts and stops it and reports what it
// is doing.
#include <algorithm>

#include "app/app.h"
#include "app/home.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/str.h"
#include "library/library.h"
#include "net/server.h"
#include "platform/net.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ck {
namespace {

class TransferScreen : public View {
 public:
  std::string title() const override { return "Send over Wi-Fi"; }
  Refresh refresh_hint() const override { return Refresh::Text; }
  // The address has to be readable, and a page that repaints every second
  // on e-ink is not; a five-second tick is enough to show arrivals.
  int tick_ms() const override { return 5000; }

  void on_show() override {
    // Nothing listens until there is a network to listen on.
    if (Net::instance().connected() && !TransferServer::instance().running()) {
      TransferServer::instance().start();
    }
    changes_seen_ = TransferServer::instance().change_counter();
  }

  void on_hide() override {
    // Leaving the screen closes the door: nothing should stay listening on
    // the network because a screen was left open.
    TransferServer::instance().stop();
  }

  bool on_tick() override {
    TransferServer& server = TransferServer::instance();
    int changes = server.change_counter();
    if (changes != changes_seen_) {
      changes_seen_ = changes;
      // Something landed on the drive; the next library or home screen
      // scans the directory again anyway, but a stale thumbnail for a
      // replaced file would survive, so drop the decoded ones.
      covers::clear_memory_cache();
    }
    return true;
  }

  void draw(Canvas& canvas, const Rect& bounds) override {
    const Theme& th = theme();
    hits_.clear();
    paint_background(canvas);
    StatusBarInfo info;
    int top = draw_top_bar(canvas, bounds, "Send over Wi-Fi", info);

    TransferServer& server = TransferServer::instance();
    Net& net = Net::instance();
    Rect body(bounds.x + th.padding, bounds.y + top + th.padding, bounds.w - 2 * th.padding,
              bounds.h - top - 2 * th.padding);
    int y = body.y;

    if (!net.connected()) {
      draw_centered_message(canvas, body,
                            "Connect to Wi-Fi first.\n\nSettings has a Wi-Fi entry; come "
                            "back here once it says connected.");
      return;
    }
    if (!server.running()) {
      draw_centered_message(canvas, body,
                            "The transfer server could not start.\n\nAnother program may "
                            "be using the port. Leave this screen and come back to retry.");
      return;
    }

    // The address, as large as it will go: it is meant to be typed by
    // someone holding the device in one hand and a phone in the other.
    draw_text_in(canvas, Rect(body.x, y, body.w, th.small_px + 8),
                 "Open this in a browser on the same network",
                 ui_style(th.small_px, th.muted), -1);
    y += th.small_px + 12;

    std::string address = format("http://%s:%d", net.ip_address().c_str(), server.port());
    Rect card(body.x, y, body.w, th.row_height * 2);
    if (th.gradients) {
      if (th.shadows) canvas.draw_soft_shadow(card, th.radius, 4, 36);
      canvas.fill_round_rect_gradient(card, th.radius, th.panel_top, th.panel_bottom);
      canvas.fill_gloss(card, th.radius, 80);
      canvas.draw_round_rect(card, th.radius, th.border, 2);
    } else {
      draw_panel(canvas, card, true);
    }
    draw_text_in(canvas, card, address, ui_style(th.title_px, th.fg, FontStyle::Bold), 0);
    y = card.bottom() + th.padding;

    draw_text_in(canvas, Rect(body.x, y, body.w, th.small_px + 8), "Key",
                 ui_style(th.small_px, th.muted), -1);
    y += th.small_px + 10;
    draw_text_in(canvas, Rect(body.x, y, body.w, th.title_px + 16), server.key(),
                 ui_style(th.title_px + 12, th.accent, FontStyle::Bold), 0);
    y += th.title_px + 24;
    draw_text_in(canvas, Rect(body.x, y, body.w, th.small_px + 8),
                 "The page asks for the key. Nobody else on the network can browse the "
                 "drive without it.",
                 ui_style(th.small_px, th.muted), 0);
    y += th.small_px + th.padding;

    draw_text_in(canvas, Rect(body.x, y, body.w, th.small_px + 8),
                 format("%d received, %d sent", server.upload_count(), server.download_count()),
                 ui_style(th.small_px, th.muted), -1);
    y += th.small_px + 12;

    std::vector<TransferServer::Activity> activity = server.recent_activity();
    if (activity.empty()) {
      draw_text_in(canvas, Rect(body.x, y, body.w, th.base_px + 10),
                   "Nothing yet. Drag a book onto the page to send it here.",
                   ui_style(th.base_px, th.muted), -1);
      y += th.base_px + 10;
    } else {
      for (const TransferServer::Activity& a : activity) {
        if (y + th.base_px + 8 > body.bottom() - th.row_height * 2) break;
        draw_text_in(canvas, Rect(body.x, y, body.w, th.base_px + 8),
                     ellipsize(a.text, ui_style(th.base_px, th.fg), body.w),
                     ui_style(th.base_px, th.fg), -1);
        y += th.base_px + 8;
      }
    }

    Rect done(body.x, body.bottom() - th.row_height, body.w, th.row_height);
    draw_button(canvas, done, "Stop sharing", ButtonStyle::Primary);
    hits_.add(done, 1);
  }

  bool handle(const InputEvent& event) override {
    if (event.type == EventType::KeyDown &&
        (event.key == Key::PageBack || event.key == Key::PageForward)) {
      App::instance().pop();
      return true;
    }
    if (event.type != EventType::Tap) return false;
    if (hits_.hit(event.x, event.y) == 1) {
      App::instance().pop();
      return true;
    }
    return false;
  }

 private:
  HitList hits_;
  int changes_seen_ = 0;
};

}  // namespace

ViewPtr make_transfer_screen() { return ViewPtr(new TransferScreen()); }

}  // namespace ck
