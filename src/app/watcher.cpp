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
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"

namespace ck {
namespace {

// Nickel opens a file several times while it works out what it is, so a
// tap can arrive as a burst. One launch per couple of seconds is plenty.
const int64_t kDebounceMs = 3000;

// Waits for the user partition to come back, which it does every time the
// stock software finishes sharing the drive with a computer.
bool wait_for_mount(const std::string& dir) {
  for (int i = 0; i < 600; ++i) {
    std::string mounts;
    if (fs::read_file("/proc/mounts", mounts) &&
        mounts.find(" " + dir + " ") != std::string::npos) {
      return true;
    }
    sleep_ms(1000);
  }
  return false;
}

}  // namespace

std::string watcher_command_for(const std::vector<Trigger>& triggers,
                                const std::string& filename) {
  for (const Trigger& trigger : triggers) {
    if (filename == trigger.filename) return trigger.args;
  }
  return "\xff";  // sentinel: not a trigger at all
}

bool write_trigger_files(const std::string& dir, const std::vector<Trigger>& triggers) {
  bool all = true;
  for (const Trigger& trigger : triggers) {
    if (trigger.blurb.empty()) continue;   // recognised, never created
    std::string path = dir + "/" + trigger.filename;
    if (fs::exists(path)) continue;
    if (!fs::write_file_atomic(path, trigger.blurb)) {
      CK_LOGW("watcher: could not write %s", path.c_str());
      all = false;
    }
  }
  return all;
}

int run_watcher(const std::string& launcher, const std::vector<Trigger>& triggers) {
  const std::string dir = paths().onboard;

  int fd = inotify_init();
  if (fd < 0) {
    CK_LOGE("watcher: inotify_init failed: %s", strerror(errno));
    return 1;
  }

  int64_t last_launch = 0;
  std::vector<char> buffer(4096);
  int wd = -1;
  while (true) {
    if (wd < 0) {
      // Watch the directory rather than the files: the stock software
      // replaces a file it has indexed, and a watch on the inode would go
      // quiet. IN_OPEN on the directory reports opens of everything in it,
      // with the name.
      write_trigger_files(dir, triggers);
      wd = inotify_add_watch(fd, dir.c_str(), IN_OPEN | IN_UNMOUNT);
      if (wd < 0) {
        CK_LOGW("watcher: cannot watch %s (%s), waiting for the mount",
                dir.c_str(), strerror(errno));
        if (!wait_for_mount(dir)) {
          CK_LOGE("watcher: %s never came back", dir.c_str());
          close(fd);
          return 1;
        }
        continue;
      }
      CK_LOGI("watcher: watching %s for %zu triggers", dir.c_str(), triggers.size());
    }

    ssize_t n = read(fd, buffer.data(), buffer.size());
    if (n <= 0) {
      if (errno == EINTR) continue;
      CK_LOGE("watcher: read failed: %s", strerror(errno));
      break;
    }
    for (ssize_t offset = 0; offset + (ssize_t)sizeof(struct inotify_event) <= n;) {
      struct inotify_event* event = (struct inotify_event*)(buffer.data() + offset);
      offset += (ssize_t)sizeof(struct inotify_event) + (ssize_t)event->len;
      // Sharing the drive over USB unmounts the partition, which takes the
      // watch with it. Without re-arming, the library entries would stop
      // working after the first time the Kobo was plugged into a computer.
      if (event->mask & (IN_UNMOUNT | IN_IGNORED)) {
        CK_LOGI("watcher: %s went away, waiting for it to come back", dir.c_str());
        inotify_rm_watch(fd, wd);
        wd = -1;
        wait_for_mount(dir);
        break;
      }
      if (event->len == 0) continue;
      std::string name(event->name);
      std::string args = watcher_command_for(triggers, name);
      if (args == "\xff") continue;
      if (now_ms() - last_launch < kDebounceMs) continue;
      last_launch = now_ms();
      std::string command = launcher + " " + args + " >/dev/null 2>&1 &";
      CK_LOGI("watcher: %s opened, running %s", name.c_str(), command.c_str());
      if (system(command.c_str()) != 0) CK_LOGW("watcher: launch failed");
    }
  }
  if (wd >= 0) inotify_rm_watch(fd, wd);
  close(fd);
  return 0;
}

}  // namespace ck
