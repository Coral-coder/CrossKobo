#pragma once
#include <functional>
#include <string>
#include <vector>

namespace ck {

// Finding catalogue servers on the local network.
//
// There is no single discovery protocol every library server speaks -
// Shelfmark, Calibre-Web, Kavita, Komga and BookLore each have their own
// idea, and mDNS is not answered by most of them. What they do have in
// common is an OPDS feed on a predictable port and path, so this sweeps the
// local subnet for one. It only ever runs when the user asks: a scan of
// someone's own network is their business, not something to do in the
// background.
struct DiscoveredCatalogue {
  std::string name;      // the feed's own title, when it has one
  std::string url;       // the OPDS feed that answered
  std::string host;      // host:port, for display
};

// Sweeps the /24 around this device's own address. `progress` is called with
// (probed, total) so a screen can say how far along it is; return false from
// it to stop early.
std::vector<DiscoveredCatalogue> discover_catalogues(
    int per_host_timeout_ms = 250,
    const std::function<bool(int done, int total)>& progress = nullptr);

// The ports and paths a sweep tries, exposed for tests and for the
// documentation to stay honest about what is probed.
const std::vector<int>& discovery_ports();
const std::vector<std::string>& discovery_paths();

}  // namespace ck
