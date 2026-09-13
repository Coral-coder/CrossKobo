#include "net/discover.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>

#include "core/log.h"
#include "core/str.h"
#include "net/http.h"
#include "net/opds.h"
#include "platform/net.h"

namespace ck {
namespace {

// How many connects are in flight at once. High enough to sweep a /24 in a
// couple of seconds, low enough not to exhaust the descriptor table.
const int kBatch = 48;

struct Probe {
  int fd = -1;
  std::string ip;
  int port = 0;
};

// Opens a non-blocking connect and returns the descriptor, or -1.
int begin_connect(const std::string& ip, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  int rv = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
  if (rv == 0) return fd;                 // connected immediately (localhost)
  if (errno == EINPROGRESS) return fd;
  close(fd);
  return -1;
}

// True when a non-blocking connect finished successfully.
bool connect_succeeded(int fd) {
  int err = 0;
  socklen_t len = sizeof(err);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return false;
  return err == 0;
}

std::string subnet_of(const std::string& ip) {
  size_t dot = ip.find_last_of('.');
  if (dot == std::string::npos) return "";
  return ip.substr(0, dot + 1);
}

}  // namespace

const std::vector<int>& discovery_ports() {
  // The defaults of the servers people actually run: Calibre-Web (8083),
  // Calibre's own content server (8080), Kavita (5000), Komga (8080/25600),
  // BookLore (6060), Audiobookshelf (13378), Shelfmark and the rest of the
  // Docker crowd (8084, 8090, 3000).
  static const std::vector<int> kPorts = {8083, 8080, 5000, 25600, 6060,
                                          8084, 8090, 3000, 13378, 80};
  return kPorts;
}

const std::vector<std::string>& discovery_paths() {
  static const std::vector<std::string> kPaths = {
      "/opds",                  // Calibre-Web, Calibre, most others
      "/opds/v1.2/catalog",     // Komga
      "/api/opds",              // Kavita and several .NET servers
      "/opds/root.xml",         // older Calibre-Web
      "/catalog",               // COPS and friends
  };
  return kPaths;
}

std::vector<DiscoveredCatalogue> discover_catalogues(
    int per_host_timeout_ms, const std::function<bool(int, int)>& progress) {
  std::vector<DiscoveredCatalogue> found;
  std::string own_ip = Net::instance().ip_address();
  std::string prefix = subnet_of(own_ip);
  if (prefix.empty()) {
    CK_LOGW("discover: no local address, nothing to sweep");
    return found;
  }
  CK_LOGI("discover: sweeping %s0/24", prefix.c_str());

  // Phase one: which addresses answer on a catalogue-shaped port at all.
  // A TCP connect is cheap and tells us where to spend an HTTP request.
  std::vector<std::pair<std::string, int>> open_ports;
  const std::vector<int>& ports = discovery_ports();
  int total = 254 * (int)ports.size();
  int done = 0;
  bool cancelled = false;

  for (size_t p = 0; p < ports.size() && !cancelled; ++p) {
    for (int base = 1; base <= 254 && !cancelled; base += kBatch) {
      std::vector<Probe> probes;
      for (int i = base; i < base + kBatch && i <= 254; ++i) {
        std::string ip = prefix + format("%d", i);
        if (ip == own_ip) continue;
        int fd = begin_connect(ip, ports[p]);
        if (fd >= 0) probes.push_back({fd, ip, ports[p]});
      }
      if (probes.empty()) continue;

      std::vector<struct pollfd> pfds;
      pfds.reserve(probes.size());
      for (const Probe& probe : probes) {
        struct pollfd pfd;
        pfd.fd = probe.fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        pfds.push_back(pfd);
      }
      poll(pfds.data(), (unsigned int)pfds.size(), per_host_timeout_ms);
      for (size_t i = 0; i < probes.size(); ++i) {
        if ((pfds[i].revents & POLLOUT) && connect_succeeded(probes[i].fd)) {
          open_ports.push_back({probes[i].ip, probes[i].port});
        }
        close(probes[i].fd);
      }
      done += (int)probes.size();
      if (progress && !progress(std::min(done, total), total)) cancelled = true;
    }
  }
  CK_LOGI("discover: %zu open ports", open_ports.size());

  // Phase two: ask each one for a feed. The first path that parses as OPDS
  // is the catalogue; anything else on that port is somebody else's server.
  for (const auto& endpoint : open_ports) {
    if (cancelled) break;
    for (const std::string& path : discovery_paths()) {
      std::string url = format("http://%s:%d%s", endpoint.first.c_str(), endpoint.second,
                               path.c_str());
      OpdsFeed feed;
      std::string error;
      if (!fetch_opds(url, "", "", feed, error)) continue;
      // A page that parses but offers nothing is not worth listing.
      if (feed.entries.empty() && feed.title.empty()) continue;
      DiscoveredCatalogue hit;
      hit.url = url;
      hit.host = format("%s:%d", endpoint.first.c_str(), endpoint.second);
      hit.name = feed.title.empty() ? hit.host : feed.title;
      found.push_back(hit);
      CK_LOGI("discover: %s -> \"%s\"", url.c_str(), hit.name.c_str());
      break;
    }
    if (progress && !progress(total, total)) cancelled = true;
  }
  return found;
}

}  // namespace ck
