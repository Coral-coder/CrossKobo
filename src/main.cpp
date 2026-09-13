// CrossKobo entry point.
//
// On the device this process is started by the boot hook after the stock
// UI's startup has settled, takes over the framebuffer and the input
// devices, and runs until the user asks to sleep, restart, or hand control
// back to the Kobo software.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "app/app.h"
#include "app/home.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "core/version.h"
#include "library/library.h"
#include "platform/device.h"
#include "platform/input.h"
#include "platform/power.h"
#include "platform/screen.h"
#include "platform/system.h"
#include "reader/state.h"
#include "ui/theme.h"

namespace {

void print_usage() {
  printf(
      "CrossKobo %s - a reading and note-taking shell for Kobo e-readers\n"
      "\n"
      "Usage: crosskobo [options]\n"
      "  --sim[=WxH]      run headless against an in-memory framebuffer\n"
      "  --root DIR       treat DIR as the user storage root (testing)\n"
      "  --book FILE      open a book straight away\n"
      "  --keep-nickel    do not stop the stock Kobo UI (debugging over ssh)\n"
      "  --debug          verbose logging\n"
      "  --version        print the version and exit\n",
      ck::kVersion);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace ck;

  bool simulate = false;
  int sim_w = 1264, sim_h = 1680;
  bool keep_nickel = false;
  bool debug = false;
  std::string root;
  std::string book;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--version") {
      printf("crosskobo %s\n", kVersion);
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--sim") {
      simulate = true;
    } else if (starts_with(arg, "--sim=")) {
      simulate = true;
      std::vector<std::string> parts = split(arg.substr(6), 'x');
      if (parts.size() == 2) {
        sim_w = to_int(parts[0], sim_w);
        sim_h = to_int(parts[1], sim_h);
      }
    } else if (arg == "--root" && i + 1 < argc) {
      root = argv[++i];
    } else if (arg == "--book" && i + 1 < argc) {
      book = argv[++i];
    } else if (arg == "--keep-nickel") {
      keep_nickel = true;
    } else if (arg == "--debug") {
      debug = true;
    } else {
      fprintf(stderr, "crosskobo: unknown option '%s'\n", arg.c_str());
      print_usage();
      return 2;
    }
  }

  // Paths: the defaults match an installed device; --root redirects
  // everything under one directory for testing on a PC.
  Paths p;
  if (!root.empty()) {
    p.onboard = root;
    p.data = root + "/.crosskobo";
    p.notebooks = root + "/Notebooks";
    p.fonts_user = root + "/fonts";
    p.install = fs::dirname(argv[0]);
    p.fonts_bundled = fs::join_path(p.install, "fonts");
    if (!fs::is_dir(p.fonts_bundled)) p.fonts_bundled = "assets/fonts";
  }
  set_paths(p);
  fs::mkdir_p(paths().data);
  fs::mkdir_p(paths().data + "/books");
  fs::mkdir_p(paths().cache_dir());
  fs::mkdir_p(paths().notebooks);

  log_init(paths().log_file(), debug ? LogLevel::Debug : LogLevel::Info);
  CK_LOGI("crosskobo %s starting", kVersion);
  CK_LOGI("device: %s", device().describe().c_str());

  // The user's escape hatch: if this file exists we are not supposed to be
  // running at all, so get out of the way immediately.
  if (fs::exists(paths().disable_flag())) {
    CK_LOGW("DISABLE flag present at %s, handing back to the stock UI",
            paths().disable_flag().c_str());
    if (!simulate && device().is_kobo) sys::start_nickel();
    return kExitReturnToNickel;
  }

  if (!simulate && device().is_kobo && !keep_nickel) {
    // If the stock UI is up (either because the boot watchdog is disabled or
    // because CrossKobo was started by hand) take its environment before
    // stopping it: restarting it later goes more smoothly with the same
    // variables. When CrossKobo owns the boot there is nothing to take, and
    // the device probe has worked everything out for itself.
    if (sys::nickel_running()) {
      sys::capture_nickel_env();
      sys::stop_nickel();
    }
  }

  App& app = App::instance();
  if (!app.init(simulate, sim_w, sim_h)) {
    CK_LOGE("crosskobo: initialisation failed");
    if (!simulate && device().is_kobo) sys::start_nickel();
    return kExitError;
  }

  // The screen is ours now. Tell the boot hook to stop animating, and stop
  // the firmware's animation processes directly: the hook honours the flag,
  // but on firmware 5 the animation is started by the system's own scripts
  // and nothing else will ever kill it once Nickel is gone.
  if (!simulate) {
    sys::signal_ready(true);
    sys::stop_boot_animation();
    // The firmware blinks the indicator LED until the stock software says
    // it is up. Nothing will say that now, so quieten it ourselves.
    Power::instance().stop_boot_led();
  }

  Stats::instance().load();
  Recents::instance().load();

  app.push(make_home_screen());
  if (!book.empty()) open_book(book);

  int code = app.run();
  app.shutdown();
  if (!simulate) sys::signal_ready(false);

  if (code == kExitReturnToNickel && !simulate && device().is_kobo) {
    sys::start_nickel();
  }
  CK_LOGI("crosskobo: exit %d", code);
  return code;
}
