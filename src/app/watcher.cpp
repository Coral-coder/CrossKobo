// The launcher watcher: how CrossKobo appears inside the stock Kobo
// software without patching it.
//
// Nickel is a closed Qt application; its menus and its bottom bar cannot be
// extended from outside it. What it does do is open files when you tap them
// in the library - so CrossKobo puts a plainly named text file on the drive
// and watches for nickel opening it. Tapping that "book" starts the
// catalogue browser, which borrows the screen and hands it straight back.
//
// This is the same mechanism KFMon has used for years, implemented here so
// the add-on needs nothing else installed.
#include "app/watcher.h"

#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"

namespace ck {
namespace {

// One trigger per thing worth launching. The names are what the reader sees
// in the stock library, so they have to read as menu entries.
struct Trigger {
  const char* filename;
  const char* args;
  // Whether the watcher creates this one when it is missing. The full
  // interface's trigger is recognised but never created, so the add-on does
  // not put an entry for it on someone's drive uninvited.
  bool create;
};

const Trigger kTriggers[] = {
    {"CrossKobo Catalogues.txt", "--catalogues", true},
    {"CrossKobo Shelfmark.txt", "--catalogue Shelfmark", true},
    {"CrossKobo.txt", "", false},
};

// Nickel opens a file several times while it works out what it is, so a
// tap can arrive as a burst. One launch per couple of seconds is plenty.
const int64_t kDebounceMs = 3000;

}  // namespace

std::string watcher_command_for(const std::string& filename) {
  for (const Trigger& trigger : kTriggers) {
    if (filename == trigger.filename) return trigger.args;
  }
  return "\xff";  // sentinel: not a trigger at all
}

std::vector<std::string> watcher_trigger_names() {
  std::vector<std::string> names;
  for (const Trigger& trigger : kTriggers) names.push_back(trigger.filename);
  return names;
}

bool write_trigger_files(const std::string& dir) {
  bool all = true;
  for (const Trigger& trigger : kTriggers) {
    if (!trigger.create) continue;
    std::string path = dir + "/" + trigger.filename;
    if (fs::exists(path)) continue;
    std::string body =
        "CrossKobo\n"
        "=========\n\n"
        "Open this from your Kobo's library and CrossKobo starts.\n\n";
    if (std::string(trigger.args).find("catalogue") != std::string::npos) {
      body +=
          "This one opens the catalogue browser: your OPDS libraries, and the\n"
          "public-domain catalogues CrossKobo ships with.\n\n"
          "Books you download land in the Downloads folder on this drive,\n"
          "where your Kobo finds them like anything copied over USB.\n\n";
    }
    body +=
        "Leave CrossKobo with a swipe up from the bottom edge, or a\n"
        "page-turn button, and the Kobo software comes straight back.\n\n"
        "Deleting this file removes the entry; the add-on puts it back on the\n"
        "next restart unless you remove the add-on too.\n";
    if (!fs::write_file_atomic(path, body)) {
      CK_LOGW("watcher: could not write %s", path.c_str());
      all = false;
    }
  }
  return all;
}

int run_watcher(const std::string& launcher) {
  const std::string dir = paths().onboard;
  write_trigger_files(dir);

  int fd = inotify_init();
  if (fd < 0) {
    CK_LOGE("watcher: inotify_init failed: %s", strerror(errno));
    return 1;
  }
  // Watch the directory rather than the files: nickel replaces a file it
  // has indexed, and a watch on the inode would go quiet. IN_OPEN on the
  // directory reports opens of everything in it, with the name.
  int wd = inotify_add_watch(fd, dir.c_str(), IN_OPEN);
  if (wd < 0) {
    CK_LOGE("watcher: cannot watch %s: %s", dir.c_str(), strerror(errno));
    close(fd);
    return 1;
  }
  CK_LOGI("watcher: watching %s for %zu triggers", dir.c_str(),
          watcher_trigger_names().size());

  int64_t last_launch = 0;
  std::vector<char> buffer(4096);
  while (true) {
    ssize_t n = read(fd, buffer.data(), buffer.size());
    if (n <= 0) {
      if (errno == EINTR) continue;
      CK_LOGE("watcher: read failed: %s", strerror(errno));
      break;
    }
    for (ssize_t offset = 0; offset + (ssize_t)sizeof(struct inotify_event) <= n;) {
      struct inotify_event* event = (struct inotify_event*)(buffer.data() + offset);
      offset += (ssize_t)sizeof(struct inotify_event) + (ssize_t)event->len;
      if (event->len == 0) continue;
      std::string name(event->name);
      std::string args = watcher_command_for(name);
      if (args == "\xff") continue;
      if (now_ms() - last_launch < kDebounceMs) continue;
      last_launch = now_ms();
      std::string command = launcher + " " + args + " >/dev/null 2>&1 &";
      CK_LOGI("watcher: %s opened, running %s", name.c_str(), command.c_str());
      if (system(command.c_str()) != 0) {
        CK_LOGW("watcher: launch failed");
      }
    }
  }
  inotify_rm_watch(fd, wd);
  close(fd);
  return 0;
}

}  // namespace ck
