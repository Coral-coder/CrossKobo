#include "app/app.h"

#include <algorithm>

#include <cmath>

#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "core/paths.h"
#include "gfx/font.h"
#include "platform/device.h"
#include "platform/power.h"
#include "platform/system.h"
#include "library/library.h"
#include "reader/state.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ck {
namespace {

// Refresh modes ranked by strength, so the strongest hint of a frame wins.
int refresh_rank(Refresh r) {
  switch (r) {
    case Refresh::Pen: return 0;
    case Refresh::Fast: return 1;
    case Refresh::Text: return 2;
    case Refresh::Highlight: return 3;
    case Refresh::Image: return 4;
    case Refresh::Color: return 5;
    case Refresh::Flash: return 6;
    case Refresh::Init: return 7;
    case Refresh::Auto:
    default: return 2;
  }
}

}  // namespace

App& App::instance() {
  static App app;
  return app;
}

bool App::init(bool simulate, int sim_width, int sim_height) {
  simulated_ = simulate;
  Screen& screen = Screen::instance();
  bool ok = simulate ? screen.open_headless(sim_width, sim_height) : screen.open();
  if (!ok) {
    CK_LOGE("app: no display available");
    return false;
  }

  settings().load();
  apply_settings();

  std::vector<std::string> font_dirs = {paths().fonts_bundled, paths().fonts_user};
  if (fs::is_dir(paths().fonts_nickel)) font_dirs.push_back(paths().fonts_nickel);
  FontManager::instance().scan(font_dirs);
  if (FontManager::instance().family_names().empty()) {
    CK_LOGE("app: no fonts found in %s", paths().fonts_bundled.c_str());
    return false;
  }

  Power::instance().init();
  if (!simulate) {
    Input& input = Input::instance();
    input.set_screen_size(screen.width(), screen.height());
    input.set_touch_transform(settings().touch_transform);
    input.set_pen_transform(settings().pen_transform);
    input.set_rotation(settings().rotation);
    input.open();
  }
  last_activity_ms_ = now_ms();
  return true;
}

void App::apply_settings() {
  Screen& screen = Screen::instance();
  const Settings& s = settings();
  screen.set_rotation((Rotation)s.rotation);
  screen.set_night_mode(s.night_mode);
  screen.set_cfa_mode(s.cfa_mode);
  screen.set_saturation_boost(s.saturation_boost);
  screen.set_flash_interval(s.refresh_frequency);
  refresh_theme_from_settings(screen.dpi());
  Input::instance().set_screen_size(screen.width(), screen.height());
  Input::instance().set_rotation(s.rotation);
  Input::instance().set_touch_transform(s.touch_transform);
  Input::instance().set_pen_transform(s.pen_transform);

  Power& power = Power::instance();
  if (power.has_frontlight()) {
    power.set_brightness(s.frontlight_on ? s.frontlight_brightness : 0);
    if (power.has_warmth()) power.set_warmth(s.frontlight_warmth);
  }
}

Rect App::screen_bounds() const { return Screen::instance().bounds(); }

void App::push(ViewPtr view) {
  if (!view) return;
  if (!stack_.empty()) stack_.back()->on_hide();
  stack_.push_back(std::move(view));
  stack_.back()->on_show();
  invalidate(Refresh::Text);
}

void App::pop() {
  if (stack_.size() <= 1) return;
  stack_.back()->on_hide();
  retired_.push_back(std::move(stack_.back()));
  stack_.pop_back();
  stack_.back()->on_show();
  invalidate(Refresh::Text);
}

void App::release_retired() { retired_.clear(); }

void App::replace(ViewPtr view) {
  if (!view) return;
  if (!stack_.empty()) {
    stack_.back()->on_hide();
    retired_.push_back(std::move(stack_.back()));
    stack_.pop_back();
  }
  stack_.push_back(std::move(view));
  stack_.back()->on_show();
  invalidate(Refresh::Text);
}

void App::pop_to_root() {
  while (stack_.size() > 1) {
    stack_.back()->on_hide();
    retired_.push_back(std::move(stack_.back()));
    stack_.pop_back();
  }
  if (!stack_.empty()) stack_.back()->on_show();
  invalidate(Refresh::Text);
}

View* App::top() { return stack_.empty() ? nullptr : stack_.back().get(); }

void App::invalidate(Refresh hint) {
  needs_draw_ = true;
  if (refresh_rank(hint) > refresh_rank(pending_hint_)) pending_hint_ = hint;
}

void App::quit(int code) {
  quit_requested_ = true;
  exit_code_ = code;
}

void App::return_to_kobo_ui() {
  quit_requested_ = true;
  exit_code_ = kExitReturnToNickel;
}

void App::show_toast(const std::string& message, int ms) {
  toast_text_ = message;
  toast_until_ms_ = now_ms() + ms;
  invalidate(Refresh::Fast);
}

void App::note_activity() { last_activity_ms_ = now_ms(); }

void App::draw_toast(Canvas& canvas) {
  if (toast_text_.empty() || now_ms() > toast_until_ms_) return;
  const Theme& th = theme();
  Rect bounds = screen_bounds();
  TextStyle st = ui_style(th.base_px, th.bg);
  int w = std::min(bounds.w - 4 * th.padding, text_width(toast_text_, st) + 4 * th.padding);
  int h = th.row_height;
  Rect box((bounds.w - w) / 2, bounds.bottom() - h - 2 * th.padding, w, h);
  canvas.fill_round_rect(box, th.radius, th.fg);
  draw_text_in(canvas, box, toast_text_, st, 0);
}

void App::draw_frame() {
  Screen& screen = Screen::instance();
  Canvas& canvas = screen.canvas();
  View* view = top();
  if (!view) return;

  if (view->opaque()) canvas.clear(theme().bg);
  canvas.clear_clip();
  view->draw(canvas, screen.bounds());
  draw_toast(canvas);

  Refresh mode = pending_hint_;
  if (mode == Refresh::Auto) mode = view->refresh_hint();
  if (screen.flash_due()) {
    mode = Refresh::Flash;
    screen.reset_flash_counter();
  }
  screen.flush(screen.bounds(), mode, false);
  needs_draw_ = false;
  pending_hint_ = Refresh::Auto;
}

void App::render_now() { draw_frame(); }

void App::draw_sleep_screen() {
  Screen& screen = Screen::instance();
  Canvas& canvas = screen.canvas();
  const Theme& th = theme();
  canvas.clear(th.bg);
  Rect b = screen.bounds();

  const Settings& s = settings();
  const std::vector<RecentEntry>& recents = Recents::instance().entries();
  const RecentEntry* book = recents.empty() ? nullptr : &recents.front();

  switch (s.sleep_screen) {
    case SleepScreen::Blank:
      break;

    case SleepScreen::Custom: {
      // Either the configured path, or a sleep.png the user dropped next
      // to their settings, which needs no file picker.
      std::string image_path = s.sleep_custom_image;
      if (image_path.empty()) image_path = paths().data + "/sleep.png";
      Canvas image;
      if (Canvas::load_image(image_path, image)) {
        // Fill the screen, cropping rather than distorting.
        double scale = std::max((double)b.w / image.width(), (double)b.h / image.height());
        int w = (int)(image.width() * scale);
        int h = (int)(image.height() * scale);
        canvas.blit_scaled(image, Rect((b.w - w) / 2, (b.h - h) / 2, w, h));
      } else {
        draw_centered_message(canvas, b, "CrossKobo", th.title_px);
      }
      break;
    }

    case SleepScreen::Cover: {
      const Canvas* cover = book ? covers::thumbnail(book->path, b.w, b.h) : nullptr;
      if (cover && cover->valid()) {
        // Cover art, scaled to fit with the title underneath if it leaves
        // room. Fitting rather than cropping keeps the artwork intact.
        double scale = std::min((double)b.w / cover->width(), (double)b.h / cover->height());
        int w = (int)(cover->width() * scale);
        int h = (int)(cover->height() * scale);
        canvas.blit_scaled(*cover, Rect((b.w - w) / 2, (b.h - h) / 2, w, h));
      } else if (book) {
        Rect card = b.inset(b.w / 6, b.h / 5);
        draw_cover(canvas, card, nullptr, book->title, book->author);
      } else {
        draw_centered_message(canvas, b, "CrossKobo", th.title_px);
      }
      break;
    }

    case SleepScreen::BookProgress: {
      if (!book) {
        draw_centered_message(canvas, b, "CrossKobo", th.title_px);
        break;
      }
      Rect middle(b.x + th.padding * 2, b.y + b.h / 2 - th.row_height * 2,
                  b.w - 4 * th.padding, th.row_height * 4);
      TextStyle title_st = ui_style(th.title_px, th.fg, FontStyle::Bold);
      std::vector<std::string> lines = wrap_text(book->title, title_st, middle.w);
      int line_h = text_height(title_st);
      int y = middle.y;
      for (size_t i = 0; i < lines.size() && i < 3; ++i) {
        draw_text_in(canvas, Rect(middle.x, y, middle.w, line_h), lines[i], title_st, 0);
        y += line_h;
      }
      if (!book->author.empty()) {
        draw_text_in(canvas, Rect(middle.x, y + 4, middle.w, th.base_px + 8), book->author,
                     ui_style(th.base_px, th.muted), 0);
        y += th.base_px + 12;
      }
      Rect bar(middle.x + middle.w / 6, y + th.padding, middle.w * 2 / 3, 6);
      draw_progress_bar(canvas, bar, book->progress, 6);
      draw_text_in(canvas, Rect(middle.x, bar.bottom() + 8, middle.w, th.base_px + 8),
                   format("%d%% read", (int)std::lround(book->progress * 100)),
                   ui_style(th.base_px, th.muted), 0);
      break;
    }

    case SleepScreen::Dashboard: {
      const Stats& stats = Stats::instance();
      struct Tile {
        const char* label;
        std::string value;
      };
      const Tile tiles[] = {
          {"Books finished", format("%d", stats.books_finished)},
          {"Time reading", human_duration(stats.total_seconds)},
          {"Pages turned", format("%d", stats.pages_turned)},
          {"Today", human_duration(stats.seconds_today())},
          {"Streak", format("%d day%s", stats.streak_days(),
                            stats.streak_days() == 1 ? "" : "s")},
      };
      draw_text_in(canvas, Rect(b.x, b.y + b.h / 6, b.w, th.title_px + 10), "CrossKobo",
                   ui_style(th.title_px, th.muted, FontStyle::Bold), 0);
      int y = b.y + b.h / 6 + th.title_px + th.row_height;
      for (const Tile& tile : tiles) {
        draw_text_in(canvas, Rect(b.x + b.w / 6, y, b.w * 2 / 3, th.row_height), tile.label,
                     ui_style(th.base_px, th.muted), -1);
        draw_text_in(canvas, Rect(b.x + b.w / 6, y, b.w * 2 / 3, th.row_height), tile.value,
                     ui_style(th.base_px, th.fg, FontStyle::Bold), 1);
        canvas.fill_rect(Rect(b.x + b.w / 6, y + th.row_height - 1, b.w * 2 / 3, 1), th.faint);
        y += th.row_height;
      }
      break;
    }
  }

  // A quiet line at the bottom, so a sleeping device does not look broken.
  if (s.sleep_screen != SleepScreen::Blank && s.sleep_screen != SleepScreen::Custom) {
    draw_text_in(canvas, Rect(b.x, b.bottom() - th.row_height, b.w, th.small_px + 8),
                 "Press the power button to wake", ui_style(th.small_px, th.faint), 0);
  }
  screen.flush(b, Refresh::Flash, true);
}

void App::sleep_now() {
  if (asleep_) return;
  CK_LOGI("app: going to sleep");
  asleep_ = true;
  sleep_started_ms_ = now_ms();
  settings().save();
  draw_sleep_screen();

  Power& power = Power::instance();
  int brightness = power.brightness();
  power.set_brightness(0);

  // The kernel call returns when the device wakes again.
  power.suspend();

  if (settings().frontlight_restore_on_wake) power.set_brightness(brightness);
  wake_up();
}

void App::wake_up() {
  if (!asleep_) return;
  asleep_ = false;
  int64_t slept_seconds = (now_ms() - sleep_started_ms_) / 1000;
  last_activity_ms_ = now_ms();
  CK_LOGI("app: awake after %lld s", (long long)slept_seconds);

  // Power off rather than resume if the device has been asleep longer than
  // the user asked for: suspend still drains a little, and a reader left in
  // a bag for a fortnight should not come out flat.
  int hours = settings().power_off_hours;
  if (hours > 0 && slept_seconds >= (int64_t)hours * 3600 && !simulated_) {
    CK_LOGI("app: asleep for %lld h, powering off", (long long)(slept_seconds / 3600));
    settings().save();
    Screen::instance().close();
    Power::instance().power_off();
    quit_requested_ = true;
    return;
  }
  Input::instance().rescan();
  Input::instance().set_screen_size(Screen::instance().width(), Screen::instance().height());
  Input::instance().drain();
  invalidate(Refresh::Flash);
}

void App::check_idle() {
  const Settings& s = settings();
  if (s.sleep_timeout_minutes <= 0 || asleep_ || simulated_) return;
  int64_t idle_ms = now_ms() - last_activity_ms_;
  if (idle_ms >= (int64_t)s.sleep_timeout_minutes * 60000) sleep_now();
}

void App::check_usb() {
  if (simulated_ || !device().is_kobo) return;
  bool plugged = sys::usb_plugged();
  if (plugged == usb_was_plugged_) return;
  usb_was_plugged_ = plugged;
  if (!plugged) return;

  const std::string& action = settings().usb_action;
  if (action == "ignore") {
    show_toast("USB connected (charging)");
    return;
  }
  if (action == "handover") {
    return_to_kobo_ui();
    return;
  }
  // Ask. The stock UI owns USB mass storage, so transferring files means
  // handing control back to it; CrossKobo returns on the next boot.
  if (confirm("USB connected",
              "Switch to the Kobo UI so your computer can see the drive?\n\n"
              "CrossKobo starts again next time you power on.",
              "Switch", "Stay")) {
    return_to_kobo_ui();
  }
}

bool App::confirm(const std::string& title, const std::string& message,
                  const std::string& ok_label, const std::string& cancel_label) {
  Screen& screen = Screen::instance();
  Canvas& canvas = screen.canvas();
  const Theme& th = theme();
  Rect bounds = screen.bounds();

  bool result = false;
  bool done = false;
  HitList hits;
  while (!done) {
    // Repaint the view underneath so the dialog composites over real content.
    if (View* view = top()) {
      canvas.clear(th.bg);
      canvas.clear_clip();
      view->draw(canvas, bounds);
    }
    TextStyle body_st = ui_style(th.base_px, th.fg);
    std::vector<std::string> lines =
        wrap_text(message, body_st, bounds.w * 3 / 4 - 2 * th.padding);
    int line_h = text_height(body_st);
    int body_h = (int)lines.size() * line_h + th.row_height + 2 * th.padding;
    Rect body = draw_dialog(canvas, bounds, title, body_h);

    int y = body.y;
    for (const std::string& line : lines) {
      draw_text_in(canvas, Rect(body.x, y, body.w, line_h), line, body_st, -1);
      y += line_h;
    }
    int button_w = (body.w - th.padding) / 2;
    Rect cancel(body.x, body.bottom() - th.row_height, button_w, th.row_height);
    Rect ok(body.right() - button_w, cancel.y, button_w, th.row_height);
    draw_button(canvas, cancel, cancel_label, ButtonStyle::Normal);
    draw_button(canvas, ok, ok_label, ButtonStyle::Primary);
    hits.clear();
    hits.add(cancel, 0);
    hits.add(ok, 1);
    screen.flush(bounds, Refresh::Text, false);

    InputEvent event = Input::instance().next(2000);
    switch (event.type) {
      case EventType::Tap: {
        int id = hits.hit(event.x, event.y);
        if (id >= 0) {
          result = id == 1;
          done = true;
        }
        break;
      }
      case EventType::KeyDown:
        if (event.key == Key::PageForward || event.key == Key::PageBack) {
          result = event.key == Key::PageForward;
          done = true;
        }
        break;
      case EventType::Timeout:
        if (simulated_) done = true;  // never block a headless run
        break;
      default:
        break;
    }
    note_activity();
  }
  invalidate(Refresh::Text);
  return result;
}

void App::show_message(const std::string& title, const std::string& message) {
  confirm(title, message, "OK", "Close");
}

void App::handle_global(const InputEvent& event) {
  if (event.type == EventType::KeyDown && event.key == Key::Power) {
    sleep_now();
    return;
  }
  if (event.type == EventType::SleepCoverClosed) {
    sleep_now();
    return;
  }
  if (event.type == EventType::KeyDown && event.key == Key::Light) {
    Power& power = Power::instance();
    power.set_frontlight_on(!power.frontlight_on());
    settings().frontlight_on = power.frontlight_on();
    settings().frontlight_brightness = power.brightness();
    show_toast(power.frontlight_on()
                   ? format("Front light %d%%", power.brightness())
                   : std::string("Front light off"));
  }
}

void App::pump(const InputEvent& event) {
  // Safe point: nothing is executing inside a view any more, so views
  // retired by the previous event can be released.
  release_retired();
  note_activity();
  if (asleep_) {
    wake_up();
    return;
  }
  handle_global(event);
  if (quit_requested_) return;
  if (View* view = top()) {
    if (view->handle(event)) invalidate();
  }
  if (!toast_text_.empty() && now_ms() > toast_until_ms_) {
    toast_text_.clear();
    invalidate(Refresh::Fast);
  }
  if (needs_draw_) draw_frame();
}

int App::run() {
  if (stack_.empty()) {
    CK_LOGE("app: nothing to show");
    return kExitError;
  }
  int64_t last_usb_check = 0;
  while (!quit_requested_) {
    // Safe point: nothing is executing inside a view any more.
    release_retired();
    if (needs_draw_) draw_frame();

    View* view = top();
    int timeout = view ? view->tick_ms() : -1;
    if (timeout < 0) timeout = 1000;
    timeout = std::min(timeout, 1000);

    InputEvent event = Input::instance().next(timeout);
    if (event.type == EventType::Timeout) {
      if (view && view->tick_ms() > 0 && view->on_tick()) invalidate();
      check_idle();
      if (now_ms() - last_usb_check > 2000) {
        last_usb_check = now_ms();
        check_usb();
      }
      if (simulated_) break;  // headless runs render one frame and stop
      continue;
    }
    pump(event);
  }

  settings().save();
  CK_LOGI("app: exiting with code %d", exit_code_);
  return exit_code_;
}

void App::shutdown() {
  stack_.clear();
  retired_.clear();
  Input::instance().close();
  Screen::instance().close();
}

}  // namespace ck
