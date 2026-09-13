#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ui/view.h"

namespace ck {

// The application shell: owns the screen, the input pump, the view stack
// and the power/idle policy.
class App {
 public:
  static App& instance();

  bool init(bool simulate, int sim_width = 1264, int sim_height = 1680);
  int run();
  void shutdown();

  void push(ViewPtr view);
  void pop();
  void replace(ViewPtr view);
  void pop_to_root();
  // When set, going back from the root view leaves CrossKobo instead of
  // doing nothing. Used by the menu launches from the stock software, where
  // one screen is the whole application.
  void set_quit_on_back(bool on) { quit_on_back_ = on; }
  // Minimal mode: the shell borrows the screen from software that is still
  // in charge of the device, so it does none of the things an interface
  // that owns the device does - no sleep timer, no USB prompt, no update
  // check, no boot-animation sweep. Used by the standalone catalogues app.
  void set_minimal(bool on) { minimal_ = on; }
  bool minimal() const { return minimal_; }
  View* top();
  size_t depth() const { return stack_.size(); }

  // Dispatches one input event exactly as the main loop does, then
  // repaints if anything changed. The loop and the test suite share this
  // so both take the same path through the code.
  void pump(const InputEvent& event);

  // Marks the screen as needing a repaint. `hint` upgrades the refresh mode
  // for this frame (the strongest hint wins).
  void invalidate(Refresh hint = Refresh::Auto);
  // Draws and flushes right now; used by views that need the panel updated
  // in the middle of handling an event.
  void render_now();

  void quit(int code = 0);
  bool quitting() const { return quit_requested_; }
  // Hands control back to the stock Kobo UI and exits.
  void return_to_kobo_ui();

  void show_toast(const std::string& message, int ms = 1800);
  void clear_toast() { toast_text_.clear(); }
  // Simple modal confirm; blocks in its own event loop.
  bool confirm(const std::string& title, const std::string& message,
               const std::string& ok_label = "OK",
               const std::string& cancel_label = "Cancel");
  void show_message(const std::string& title, const std::string& message);

  void sleep_now();
  bool asleep() const { return asleep_; }
  void note_activity();

  Rect screen_bounds() const;
  bool simulated() const { return simulated_; }

 private:
  App() = default;
  void draw_frame();
  void release_retired();
  void draw_toast(Canvas& canvas);
  void handle_global(const InputEvent& event);
  void check_idle();
  void check_usb();
  void wake_up();
  void draw_sleep_screen();
  void apply_settings();

  std::vector<ViewPtr> stack_;
  // Views popped during event handling are parked here and released at the
  // top of the next loop iteration: a row's callback usually belongs to the
  // very view being popped, and destroying it mid-callback would pull the
  // ground out from under itself.
  std::vector<ViewPtr> retired_;
  bool simulated_ = false;
  bool quit_requested_ = false;
  int exit_code_ = 0;
  bool needs_draw_ = true;
  Refresh pending_hint_ = Refresh::Auto;
  int frames_since_flash_ = 0;

  bool asleep_ = false;
  int64_t last_activity_ms_ = 0;
  int64_t sleep_started_ms_ = 0;
  bool quit_on_back_ = false;
  bool minimal_ = false;
  bool clock_synced_ = false;
  bool usb_prompt_open_ = false;
  bool usb_was_plugged_ = false;

  std::string toast_text_;
  int64_t toast_until_ms_ = 0;
};

// Exit codes the launcher script understands.
enum ExitCode {
  kExitNormal = 0,
  kExitError = 1,
  kExitReturnToNickel = 10,  // hand over to the stock UI, do not relaunch
  kExitRestart = 11,         // relaunch CrossKobo (after a settings change)
};

}  // namespace ck
