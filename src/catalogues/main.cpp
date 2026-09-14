// Catalogues: OPDS and search-format catalogues in the Kobo's own browser.
//
// This is the whole program: a web server on the loopback address, serving
// plain pages that the stock browser shows. The browser supplies the touch,
// the keyboard and the scrolling; this supplies the books. It is started by
// the menu entry, serves until nobody has asked for anything in a while,
// and exits. It never touches the screen, the input devices or the stock
// software.
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include "catalogues/httpd.h"
#include "catalogues/service.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "core/version.h"

namespace {

const int kDefaultPort = 6420;

void print_usage() {
  printf(
      "Catalogues %s - book catalogues in the Kobo's own browser\n"
      "\n"
      "Usage: catalogues [options]\n"
      "  --port N       listen on 127.0.0.1:N (default %d)\n"
      "  --daemon       detach from the terminal and run in the background\n"
      "  --idle MIN     exit after MIN minutes without a request (default 60; 0 never)\n"
      "  --root DIR     treat DIR as the Kobo drive (testing on a PC)\n"
      "  --ping         exit 0 if a server is answering on the port, 1 if not\n"
      "  --debug        verbose logging\n"
      "  --version      print the version and exit\n",
      ck::kVersion, kDefaultPort);
}

}  // namespace

int main(int argc, char** argv) {
  int port = kDefaultPort;
  int idle_minutes = 60;
  std::string root;
  bool ping = false;
  bool debug = false;
  bool daemon = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--version") {
      printf("catalogues %s\n", ck::kVersion);
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--port" && i + 1 < argc) {
      port = ck::to_int(argv[++i], kDefaultPort);
    } else if (arg == "--idle" && i + 1 < argc) {
      idle_minutes = ck::to_int(argv[++i], idle_minutes);
    } else if (arg == "--root" && i + 1 < argc) {
      root = argv[++i];
    } else if (arg == "--ping") {
      ping = true;
    } else if (arg == "--daemon") {
      daemon = true;
    } else if (arg == "--debug") {
      debug = true;
    } else {
      fprintf(stderr, "catalogues: unknown option '%s'\n", arg.c_str());
      print_usage();
      return 2;
    }
  }

  if (ping) return catalogues::ping(port) ? 0 : 1;

  // Off on its own: a new session, nothing of the launcher's kept open, so
  // the menu entry that started us can finish and the drive can be handed
  // to a computer without us in the way.
  if (daemon) {
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid > 0) return 0;
    setsid();
    if (chdir("/") != 0) return 1;
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, 0);
      dup2(null, 1);
      dup2(null, 2);
      if (null > 2) close(null);
    }
  }
  // A browser that closes a connection early must not take the server
  // with it.
  signal(SIGPIPE, SIG_IGN);

  // Its own corner of the drive, under .adds like other add-ons, and its
  // own install directory. Nothing is shared with anything else.
  ck::Paths p;
  p.install = "/usr/local/catalogues";
  p.data = "/mnt/onboard/.adds/catalogues";
  if (!root.empty()) {
    p.onboard = root;
    p.data = root + "/.adds/catalogues";
    p.install = ck::fs::dirname(argv[0]);
  }
  ck::set_paths(p);
  ck::fs::mkdir_p(ck::paths().data);

  ck::log_init(ck::paths().data + "/catalogues.log",
               debug ? ck::LogLevel::Debug : ck::LogLevel::Info);
  ck::log_keep_closed(true);
  CK_LOGI("catalogues %s starting on port %d", ck::kVersion, port);
  catalogues::ensure_catalogue_file();
  catalogues::ensure_shelfmark_file();

  // The Kobo leaves the loopback interface unconfigured until its Wi-Fi
  // scripts run, and a server cannot bind to an address the kernel does
  // not have. On a PC this is a no-op.
  if (!catalogues::ensure_loopback()) {
    CK_LOGW("catalogues: could not set up the loopback interface; trying anyway");
  }

  bool ok = catalogues::serve("127.0.0.1", port, catalogues::handle, nullptr,
                              idle_minutes > 0 ? idle_minutes * 60 * 1000 : 0);
  CK_LOGI("catalogues: exit");
  return ok ? 0 : 1;
}
