#pragma once
#include <functional>
#include <string>

namespace ck {

// Software updates. CrossKobo installs the way the firmware already
// understands: a KoboRoot.tgz dropped in .kobo on the user partition, which
// the Kobo unpacks on the next boot. So updating from inside the app means
// fetching the release package and staging that file - no privileged
// installer, and the same result as unpacking the zip from a computer.
struct UpdateInfo {
  std::string version;     // "0.1.4"
  std::string tag;         // "v0.1.4"
  std::string asset_url;   // the install zip for this firmware generation
  std::string asset_name;
  std::string notes;       // release body, trimmed
  int64_t size = 0;
  bool newer = false;      // newer than the running build
};

// Compares dotted versions. Returns <0, 0 or >0 like strcmp.
int compare_versions(const std::string& a, const std::string& b);

// Asks the release host what the latest version is. Blocking; needs the
// network up. Returns false and sets `error` on failure.
bool check_for_update(UpdateInfo& out, std::string& error);

// Downloads the package and stages KoboRoot.tgz in .kobo. After this the
// device installs on its next restart.
bool stage_update(const UpdateInfo& info, std::string& error,
                  std::function<void(int64_t, int64_t)> progress = nullptr);

// True when an update has already been staged and is waiting for a restart.
bool update_staged();

// Background check, run at most once a day and only when Wi-Fi is already
// connected, so it never delays start-up or wakes the radio by itself.
void start_background_check();
// Takes the result of a finished background check, if there is one. Returns
// false while the check is still running or when there was nothing to say.
bool take_background_result(UpdateInfo& out);

}  // namespace ck
