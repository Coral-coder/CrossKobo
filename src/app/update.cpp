#include "app/update.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include "core/clock.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "core/version.h"
#include "epub/zip.h"
#include "net/http.h"
#include "platform/device.h"
#include "platform/net.h"

namespace ck {
namespace {

// The releases live on GitHub, and the API answer is small and stable.
const char* kLatestUrl = "https://api.github.com/repos/Coral-coder/CrossKobo/releases/latest";
const char* kStamp = "update-check";

std::vector<int> version_parts(const std::string& text) {
  std::vector<int> parts;
  std::string digits;
  for (char ch : text) {
    if (ch >= '0' && ch <= '9') {
      digits += ch;
    } else if (ch == '.') {
      parts.push_back(to_int(digits, 0));
      digits.clear();
    } else if (!digits.empty()) {
      break;  // stop at a suffix like "-rc1"
    }
  }
  if (!digits.empty()) parts.push_back(to_int(digits, 0));
  return parts;
}

std::string stamp_path() { return paths().data + "/" + kStamp; }

// Where the background checker leaves what it found. In /tmp, so a result
// never outlives the session that fetched it.
const char* kResultPath = "/tmp/crosskobo-update.json";

}  // namespace

int compare_versions(const std::string& a, const std::string& b) {
  std::vector<int> pa = version_parts(a);
  std::vector<int> pb = version_parts(b);
  size_t n = std::max(pa.size(), pb.size());
  for (size_t i = 0; i < n; ++i) {
    int va = i < pa.size() ? pa[i] : 0;
    int vb = i < pb.size() ? pb[i] : 0;
    if (va != vb) return va < vb ? -1 : 1;
  }
  return 0;
}

bool check_for_update(UpdateInfo& out, std::string& error) {
  out = UpdateInfo();
  if (!https_available()) {
    error =
        "This firmware has no curl or wget, so CrossKobo cannot fetch the "
        "release itself. Install updates from a computer instead.";
    return false;
  }
  HttpResponse response = http_get(kLatestUrl, 20000);
  if (!response.error.empty()) {
    error = response.error;
    return false;
  }
  if (!response.ok()) {
    error = format("The release host answered %d.", response.status);
    return false;
  }
  Json json;
  std::string parse_error;
  if (!Json::parse(response.body, json, &parse_error)) {
    error = "The release list could not be read: " + parse_error;
    return false;
  }
  out.tag = json.get_string("tag_name");
  out.version = out.tag;
  if (!out.version.empty() && (out.version[0] == 'v' || out.version[0] == 'V')) {
    out.version.erase(0, 1);
  }
  out.notes = json.get_string("body");
  if (out.notes.size() > 600) out.notes = out.notes.substr(0, 600) + "…";
  if (out.version.empty()) {
    error = "The release host did not name a version.";
    return false;
  }

  // Pick the package for this device's firmware generation: the firmware 5
  // layout needs the update.tar flavour, everything else the plain zip.
  bool want_fw5 = device().layout == FirmwareLayout::V5;
  const Json* assets = json.find("assets");
  if (assets && assets->is_array()) {
    for (const Json& asset : assets->items()) {
      std::string name = asset.get_string("name");
      if (name.find("install") == std::string::npos) continue;
      bool is_fw5 = name.find("fw5") != std::string::npos;
      if (is_fw5 != want_fw5) continue;
      out.asset_name = name;
      out.asset_url = asset.get_string("browser_download_url");
      out.size = asset.get_int64("size");
      break;
    }
  }
  if (out.asset_url.empty()) {
    error = "Version " + out.version + " has no package for this device.";
    return false;
  }
  out.newer = compare_versions(out.version, kVersion) > 0;
  CK_LOGI("update: latest is %s (running %s), asset %s", out.version.c_str(), kVersion,
          out.asset_name.c_str());
  fs::write_file_atomic(stamp_path(), format("%lld\n", (long long)wall_seconds()));
  return true;
}

bool stage_update(const UpdateInfo& info, std::string& error,
                  std::function<void(int64_t, int64_t)> progress) {
  if (info.asset_url.empty()) {
    error = "No package to download.";
    return false;
  }
  std::string zip_path = paths().cache_dir() + "/update.zip";
  fs::mkdir_p(paths().cache_dir());
  fs::remove_file(zip_path);
  HttpResponse response = http_download(info.asset_url, zip_path, progress);
  if (!response.error.empty() || !response.ok()) {
    fs::remove_file(zip_path);
    error = response.error.empty() ? format("Download failed (%d).", response.status)
                                   : response.error;
    return false;
  }

  // The package is exactly what a person unpacks onto the drive: pull the
  // payload out of it and put it where the firmware looks.
  ZipReader zip;
  if (!zip.open(zip_path)) {
    fs::remove_file(zip_path);
    error = "The downloaded package could not be opened.";
    return false;
  }
  struct Payload {
    const char* inside;
    std::string target;
  };
  const Payload payloads[] = {
      {".kobo/KoboRoot.tgz", paths().onboard + "/.kobo/KoboRoot.tgz"},
      {".kobo/update.tar", paths().onboard + "/.kobo/update.tar"},
  };
  bool staged = false;
  for (const Payload& payload : payloads) {
    std::string name = zip.resolve(payload.inside);
    if (name.empty()) continue;
    std::string blob;
    if (!zip.read(name, blob) || blob.empty()) continue;
    fs::mkdir_p(fs::dirname(payload.target));
    if (!fs::write_file_atomic(payload.target, blob)) {
      error = "Could not write " + payload.target + ".";
      break;
    }
    CK_LOGI("update: staged %s (%zu bytes)", payload.target.c_str(), blob.size());
    staged = true;
  }
  zip.close();
  fs::remove_file(zip_path);
  if (!staged && error.empty()) error = "The package did not contain an installer.";
  return staged;
}

bool update_staged() {
  return fs::exists(paths().onboard + "/.kobo/KoboRoot.tgz") ||
         fs::exists(paths().onboard + "/.kobo/update.tar");
}

void start_background_check() {
  // Only when the radio is already up: CrossKobo never turns Wi-Fi on by
  // itself, and a check is not worth the battery.
  if (!Net::instance().connected() || !https_available()) return;
  if (fs::exists(kResultPath)) return;  // a result is already waiting

  // At most once a day.
  std::string stamp;
  if (fs::read_file(stamp_path(), stamp)) {
    int64_t then = (int64_t)to_int(trim(stamp), 0);
    if (then > 0 && wall_seconds() - then < 24 * 3600) return;
  }

  // A child process rather than a thread: the shipping binary is statically
  // linked, the fetch already runs a helper process for TLS, and a wedged
  // network cannot then hold a thread inside the reader. The grandchild is
  // reparented to init, so nothing has to be reaped here.
  pid_t pid = fork();
  if (pid < 0) {
    CK_LOGW("update: could not fork a checker: %s", strerror(errno));
    return;
  }
  if (pid > 0) {
    int status = 0;
    waitpid(pid, &status, 0);  // the intermediate exits immediately
    return;
  }
  if (fork() != 0) _exit(0);
  UpdateInfo info;
  std::string error;
  bool ok = check_for_update(info, error);
  if (ok && info.newer) {
    Json json = Json::object();
    json["version"] = Json(info.version);
    json["tag"] = Json(info.tag);
    json["asset"] = Json(info.asset_name);
    json["url"] = Json(info.asset_url);
    json["size"] = Json((int64_t)info.size);
    json["notes"] = Json(info.notes);
    fs::write_file_atomic(kResultPath, json.dump(false));
  } else if (!ok) {
    CK_LOGW("update: background check failed: %s", error.c_str());
  }
  _exit(0);
}

bool take_background_result(UpdateInfo& out) {
  std::string text;
  if (!fs::read_file(kResultPath, text)) return false;
  fs::remove_file(kResultPath);
  Json json;
  if (!Json::parse(text, json)) return false;
  out = UpdateInfo();
  out.version = json.get_string("version");
  out.tag = json.get_string("tag");
  out.asset_name = json.get_string("asset");
  out.asset_url = json.get_string("url");
  out.size = json.get_int64("size");
  out.notes = json.get_string("notes");
  out.newer = !out.version.empty() && compare_versions(out.version, kVersion) > 0;
  return out.newer;
}

}  // namespace ck
