#include "net/sntp.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>

#include "core/clock.h"
#include "core/log.h"

namespace ck {
namespace {

// Seconds between 1900 (the NTP epoch) and 1970 (the Unix one).
const int64_t kNtpToUnix = 2208988800LL;

// A handful of servers, tried in order. The pool round-robins, so one
// failure is usually a DNS answer we could not use rather than a server
// that is down.
const char* kServers[] = {"pool.ntp.org", "time.cloudflare.com", "time.google.com"};

}  // namespace

int64_t sntp_decode(const unsigned char reply[48], std::string& error) {
  // Mode 4 is a server answering a client; 5 is a broadcast server.
  int mode = reply[0] & 0x07;
  if (mode != 4 && mode != 5) {
    error = "not a server answer";
    return 0;
  }
  // Stratum 0 is the "kiss of death": the server is telling us to go away.
  if (reply[1] == 0) {
    error = "the server refused the request";
    return 0;
  }
  // Bytes 40..43: the transmit timestamp, seconds since 1900, big-endian.
  uint32_t seconds = ((uint32_t)reply[40] << 24) | ((uint32_t)reply[41] << 16) |
                     ((uint32_t)reply[42] << 8) | (uint32_t)reply[43];
  if (seconds == 0) {
    error = "no timestamp";
    return 0;
  }
  int64_t unix_time = (int64_t)seconds - kNtpToUnix;
  // Older than September 2020 means we misread the packet, or the server is
  // not to be trusted with the clock.
  if (unix_time < 1600000000LL) {
    error = "implausible time";
    return 0;
  }
  return unix_time;
}

int64_t sntp_query(const std::string& host, int timeout_ms, std::string& error) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;          // v4 only: the device's Wi-Fi is v4
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo* result = nullptr;
  int rv = getaddrinfo(host.c_str(), "123", &hints, &result);
  if (rv != 0 || !result) {
    error = "cannot resolve " + host;
    return 0;
  }

  int fd = socket(result->ai_family, SOCK_DGRAM, 0);
  if (fd < 0) {
    freeaddrinfo(result);
    error = std::string("socket: ") + strerror(errno);
    return 0;
  }

  // An SNTP client request: leap 0, version 4, mode 3 (client). Everything
  // else stays zero, which is what a client is supposed to send.
  unsigned char packet[48] = {0};
  packet[0] = 0x23;
  int64_t sent_at = now_ms();
  ssize_t n = sendto(fd, packet, sizeof(packet), 0, result->ai_addr, result->ai_addrlen);
  freeaddrinfo(result);
  if (n != (ssize_t)sizeof(packet)) {
    close(fd);
    error = std::string("send: ") + strerror(errno);
    return 0;
  }

  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;
  int ready = poll(&pfd, 1, timeout_ms);
  if (ready <= 0) {
    close(fd);
    error = ready == 0 ? host + " did not answer" : std::string("poll: ") + strerror(errno);
    return 0;
  }
  unsigned char reply[48] = {0};
  n = recv(fd, reply, sizeof(reply), 0);
  close(fd);
  if (n < (ssize_t)sizeof(reply)) {
    error = "short answer from " + host;
    return 0;
  }

  std::string decode_error;
  int64_t unix_time = sntp_decode(reply, decode_error);
  if (unix_time == 0) {
    error = decode_error + " (" + host + ")";
    return 0;
  }
  // Half the round trip, so the answer lands closer to now than to then.
  unix_time += (now_ms() - sent_at) / 2000;
  return unix_time;
}

bool sync_clock(std::string& error, int64_t* drift_seconds) {
  for (const char* server : kServers) {
    std::string attempt_error;
    int64_t when = sntp_query(server, 2500, attempt_error);
    if (when == 0) {
      CK_LOGI("clock: %s", attempt_error.c_str());
      error = attempt_error;
      continue;
    }
    int64_t drift = when - wall_seconds();
    if (drift_seconds) *drift_seconds = drift;
    struct timeval tv;
    tv.tv_sec = (time_t)when;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
      error = std::string("cannot set the clock: ") + strerror(errno);
      return false;
    }
    // Push it into the hardware clock too, or the next boot starts from the
    // old time again. Not every firmware ships hwclock, so a failure here
    // is not a failure of the sync.
    if (system("hwclock -w -u >/dev/null 2>&1") != 0) {
      CK_LOGI("clock: hwclock unavailable, the RTC keeps its own time");
    }
    CK_LOGI("clock: set from %s, drift was %lld s", server, (long long)drift);
    error.clear();
    return true;
  }
  if (error.empty()) error = "no time server answered";
  return false;
}

}  // namespace ck
