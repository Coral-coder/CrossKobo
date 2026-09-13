#include "net/http.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"

namespace ck {
namespace {

constexpr size_t kMaxInMemoryBody = 16 * 1024 * 1024;

// ------------------------------------------------------------------ socket
class Socket {
 public:
  ~Socket() { close(); }

  bool connect(const std::string& host, int port, int timeout_ms, std::string& error) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    std::string service = format("%d", port);
    // The shipping binary is static, so glibc's getaddrinfo cannot load its
    // NSS modules: names resolve through /etc/hosts and plain DNS only.
    // That covers the local network, and https goes out through the
    // device's own curl or wget, which is dynamically linked.
    int rv = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (rv != 0 || !result) {
      error = format("cannot resolve %s (%s)", host.c_str(), gai_strerror(rv));
      return false;
    }
    for (struct addrinfo* ai = result; ai; ai = ai->ai_next) {
      fd_ = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
      if (fd_ < 0) continue;
      // Connect with a timeout: a reader on a flaky network should not hang.
      int flags = fcntl(fd_, F_GETFL, 0);
      fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
      int cr = ::connect(fd_, ai->ai_addr, ai->ai_addrlen);
      if (cr == 0 || (cr < 0 && errno == EINPROGRESS)) {
        struct pollfd p;
        p.fd = fd_;
        p.events = POLLOUT;
        p.revents = 0;
        if (poll(&p, 1, timeout_ms) == 1 && (p.revents & POLLOUT)) {
          int err = 0;
          socklen_t len = sizeof(err);
          if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
            fcntl(fd_, F_SETFL, flags);
            int one = 1;
            setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            freeaddrinfo(result);
            return true;
          }
          error = format("cannot connect to %s:%d (%s)", host.c_str(), port, strerror(err));
        } else {
          error = format("timed out connecting to %s:%d", host.c_str(), port);
        }
      } else {
        error = format("cannot connect to %s:%d (%s)", host.c_str(), port, strerror(errno));
      }
      close();
    }
    freeaddrinfo(result);
    return false;
  }

  bool write_all(const std::string& data, int timeout_ms) {
    size_t sent = 0;
    while (sent < data.size()) {
      struct pollfd p;
      p.fd = fd_;
      p.events = POLLOUT;
      p.revents = 0;
      if (poll(&p, 1, timeout_ms) != 1) return false;
      ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return false;
      }
      sent += (size_t)n;
    }
    return true;
  }

  // Returns the number of bytes read, 0 on clean end of stream, -1 on error.
  ssize_t read_some(char* buffer, size_t size, int timeout_ms) {
    struct pollfd p;
    p.fd = fd_;
    p.events = POLLIN;
    p.revents = 0;
    int pr = poll(&p, 1, timeout_ms);
    if (pr == 0) return -1;   // timeout
    if (pr < 0) return errno == EINTR ? 0 : -1;
    ssize_t n = ::recv(fd_, buffer, size, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
    return n;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

// The certificates the bundled fetcher verifies against, plus anywhere a
// reader can add their own - a home server with its own CA, say.
std::string ca_bundle() {
  // The one installed alongside this program first, whichever program it
  // is; then any other install of ours; then the firmware's own.
  std::string own = paths().install + "/cacert.pem";
  if (fs::exists(own)) return own;
  static const char* kCandidates[] = {"/usr/local/catalogues/cacert.pem",
                                      "/usr/local/crosskobo/cacert.pem",
                                      "/etc/ssl/certs/ca-certificates.crt"};
  for (const char* path : kCandidates) {
    if (fs::exists(path)) return path;
  }
  return "";
}

std::string extra_ca_bundle() {
  std::string path = paths().data + "/extra-ca.pem";
  return fs::exists(path) ? path : "";
}

// What to hand the fetcher: the bundled certificates, or - when a reader has
// added their own - the two concatenated. Passing a second --cacert would
// replace the first rather than add to it, which would quietly drop every
// public CA the moment someone trusted their own server.
std::string effective_ca_bundle() {
  std::string bundle = ca_bundle();
  std::string extra = extra_ca_bundle();
  if (extra.empty()) return bundle;
  if (bundle.empty()) return extra;

  static std::string combined_path;
  static int64_t built_at = 0;
  int64_t newest = std::max(fs::mtime(bundle), fs::mtime(extra));
  if (!combined_path.empty() && built_at >= newest && fs::exists(combined_path)) {
    return combined_path;
  }
  std::string bundle_text;
  std::string extra_text;
  if (!fs::read_file(bundle, bundle_text) || !fs::read_file(extra, extra_text)) {
    return bundle;
  }
  std::string path = "/tmp/crosskobo-ca.pem";
  if (!fs::write_file_atomic(path, bundle_text + "\n" + extra_text)) return bundle;
  combined_path = path;
  built_at = newest;
  CK_LOGI("http: trusting %s alongside the bundled certificates", extra.c_str());
  return combined_path;
}

std::string find_tls_tool() {
  // CrossKobo's own fetcher first. It is a static curl with its own TLS and
  // its own DNS resolver, so https behaves the same on every device instead
  // of depending on what the firmware happens to ship - which is nothing at
  // all on some of them.
  std::string own = paths().install + "/bin/curl";
  if (fs::exists(own)) return own;
  static const char* kCandidates[] = {"/usr/local/catalogues/bin/curl",
                                      "/usr/local/crosskobo/bin/curl",
                                      "/usr/bin/curl",
                                      "/bin/curl",
                                      "/usr/local/bin/curl",
                                      "/usr/bin/wget",
                                      "/bin/wget"};
  for (const char* path : kCandidates) {
    if (fs::exists(path)) return path;
  }
  return "";
}

// Fetches an https URL with whatever tool the device has. Not pretty, but
// honest: no TLS is linked into CrossKobo, so this is the only way.
HttpResponse perform_via_tool(const HttpRequest& request) {
  HttpResponse response;
  std::string tool = find_tls_tool();
  if (tool.empty()) {
    response.error =
        "No TLS client on this device, so https addresses cannot be fetched. "
        "Reinstall CrossKobo to get its own fetcher back, or use an http address.";
    return response;
  }
  // Worth one line in the log: which fetcher answered for https is the
  // first thing to know when a catalogue will not load.
  static std::string announced;
  if (announced != tool) {
    announced = tool;
    CK_LOGI("http: https goes through %s%s", tool.c_str(),
            tool.find("/crosskobo/") != std::string::npos ||
                    tool.find("/catalogues/") != std::string::npos
                ? " (bundled)"
                : "");
  }
  std::string target = request.download_path.empty()
                           ? "/tmp/crosskobo-https.tmp"
                           : request.download_path;
  fs::mkdir_p(fs::dirname(target));
  std::string url = request.url.to_string();
  std::string cmd;
  bool is_curl = tool.find("curl") != std::string::npos;
  if (is_curl) {
    cmd = format("%s -sSL --max-time %d -o '%s' -w '%%{http_code}'", tool.c_str(),
                 std::max(1, request.timeout_ms / 1000), target.c_str());
    // Certificates: the bundled set, plus a reader's own CA when they have
    // put one on the drive. Verification stays on either way - a catalogue
    // that needs it off is a catalogue worth knowing about.
    std::string bundle = effective_ca_bundle();
    if (!bundle.empty()) cmd += format(" --cacert '%s'", bundle.c_str());
    for (const auto& kv : request.headers) {
      cmd += format(" -H '%s: %s'", kv.first.c_str(), kv.second.c_str());
    }
    if (request.method == "POST") {
      cmd += " -X POST --data-binary @-";
    }
    cmd += " '" + url + "'";
  } else {
    cmd = format("%s -q -T %d -O '%s' '%s' && echo 200 || echo 000", tool.c_str(),
                 std::max(1, request.timeout_ms / 1000), target.c_str(), url.c_str());
  }

  FILE* pipe = request.method == "POST" && is_curl ? popen(cmd.c_str(), "we")
                                                   : popen(cmd.c_str(), "re");
  if (!pipe) {
    response.error = "could not run " + tool;
    return response;
  }
  if (request.method == "POST" && is_curl) {
    fwrite(request.body.data(), 1, request.body.size(), pipe);
    pclose(pipe);
    // The status code is lost in this mode; assume success if we got a body.
    response.status = fs::exists(target) ? 200 : 0;
  } else {
    std::string out;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe)) out += buffer;
    pclose(pipe);
    response.status = to_int(trim(out), 0);
  }
  if (request.download_path.empty()) {
    fs::read_file(target, response.body);
    fs::remove_file(target);
  }
  if (response.status == 0) response.error = "the TLS client failed";
  return response;
}

}  // namespace

// -------------------------------------------------------------------- URL

bool Url::parse(const std::string& text, Url& out) {
  std::string rest = trim(text);
  size_t scheme_end = rest.find("://");
  if (scheme_end != std::string::npos) {
    out.scheme = to_lower(rest.substr(0, scheme_end));
    rest = rest.substr(scheme_end + 3);
  } else {
    out.scheme = "http";
  }
  if (out.scheme != "http" && out.scheme != "https") return false;
  out.port = out.scheme == "https" ? 443 : 80;

  size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string path_part = slash == std::string::npos ? "/" : rest.substr(slash);

  size_t at = authority.find('@');
  if (at != std::string::npos) {
    std::string credentials = authority.substr(0, at);
    authority = authority.substr(at + 1);
    size_t colon = credentials.find(':');
    if (colon == std::string::npos) {
      out.user = credentials;
    } else {
      out.user = credentials.substr(0, colon);
      out.password = credentials.substr(colon + 1);
    }
  }
  // IPv6 literals are bracketed; everything else splits on the last colon.
  if (!authority.empty() && authority[0] == '[') {
    size_t close = authority.find(']');
    if (close == std::string::npos) return false;
    out.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') {
      out.port = to_int(authority.substr(close + 2), out.port);
    }
  } else {
    size_t colon = authority.find(':');
    if (colon == std::string::npos) {
      out.host = authority;
    } else {
      out.host = authority.substr(0, colon);
      out.port = to_int(authority.substr(colon + 1), out.port);
    }
  }
  if (out.host.empty()) return false;

  size_t question = path_part.find('?');
  if (question == std::string::npos) {
    out.path = path_part;
    out.query.clear();
  } else {
    out.path = path_part.substr(0, question);
    out.query = path_part.substr(question + 1);
  }
  size_t hash = out.query.find('#');
  if (hash != std::string::npos) out.query = out.query.substr(0, hash);
  if (out.path.empty()) out.path = "/";
  return true;
}

std::string Url::to_string() const {
  std::string out = scheme + "://" + host;
  bool default_port = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
  if (!default_port) out += format(":%d", port);
  out += path;
  if (!query.empty()) out += "?" + query;
  return out;
}

std::string HttpResponse::header(const std::string& name) const {
  auto it = headers.find(to_lower(name));
  return it == headers.end() ? "" : it->second;
}

// ------------------------------------------------------------------ client

HttpResponse http_perform(const HttpRequest& request) {
  HttpRequest current = request;
  HttpResponse response;

  for (int redirect = 0; redirect <= request.max_redirects; ++redirect) {
    if (current.url.scheme == "https") return perform_via_tool(current);

    Socket socket;
    if (!socket.connect(current.url.host, current.url.port, current.timeout_ms,
                        response.error)) {
      return response;
    }

    std::string head = current.method + " " + current.url.request_target() + " HTTP/1.1\r\n";
    head += "Host: " + current.url.host;
    if (current.url.port != 80) head += format(":%d", current.url.port);
    head += "\r\n";
    head += "User-Agent: CrossKobo\r\n";
    head += "Connection: close\r\n";
    head += "Accept-Encoding: identity\r\n";
    if (!current.url.user.empty()) {
      head += "Authorization: Basic " +
              base64_encode(current.url.user + ":" + current.url.password) + "\r\n";
    }
    for (const auto& kv : current.headers) head += kv.first + ": " + kv.second + "\r\n";
    if (!current.body.empty()) head += format("Content-Length: %zu\r\n", current.body.size());
    head += "\r\n";

    if (!socket.write_all(head + current.body, current.timeout_ms)) {
      response.error = "the connection dropped while sending the request";
      return response;
    }

    // ------------------------------------------------------------ response
    std::string buffer;
    char chunk[16384];
    size_t header_end = std::string::npos;
    while (true) {
      ssize_t n = socket.read_some(chunk, sizeof(chunk), current.timeout_ms);
      if (n < 0) {
        response.error = "the connection timed out waiting for a reply";
        return response;
      }
      if (n == 0 && buffer.empty()) {
        response.error = "the server closed the connection without replying";
        return response;
      }
      if (n > 0) buffer.append(chunk, (size_t)n);
      header_end = buffer.find("\r\n\r\n");
      if (header_end != std::string::npos) break;
      if (n == 0) {
        response.error = "the reply had no headers";
        return response;
      }
      if (buffer.size() > 256 * 1024) {
        response.error = "the reply headers were absurdly large";
        return response;
      }
    }

    std::vector<std::string> lines = split(buffer.substr(0, header_end), '\n');
    if (lines.empty()) {
      response.error = "malformed reply";
      return response;
    }
    {
      std::vector<std::string> status_parts = split(trim(lines[0]), ' ');
      response.status = status_parts.size() >= 2 ? to_int(status_parts[1]) : 0;
    }
    response.headers.clear();
    for (size_t i = 1; i < lines.size(); ++i) {
      std::string line = trim(lines[i]);
      size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      response.headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }

    // Follow redirects, keeping the method for 307/308 and switching to GET
    // otherwise, as browsers do.
    if ((response.status == 301 || response.status == 302 || response.status == 303 ||
         response.status == 307 || response.status == 308) &&
        redirect < request.max_redirects) {
      std::string location = response.header("location");
      if (!location.empty()) {
        Url next;
        if (location[0] == '/') {
          next = current.url;
          size_t question = location.find('?');
          next.path = question == std::string::npos ? location : location.substr(0, question);
          next.query = question == std::string::npos ? "" : location.substr(question + 1);
        } else if (!Url::parse(location, next)) {
          response.error = "the server redirected somewhere unparseable";
          return response;
        }
        current.url = next;
        if (response.status != 307 && response.status != 308) {
          current.method = "GET";
          current.body.clear();
        }
        continue;
      }
    }

    bool chunked = to_lower(response.header("transfer-encoding")).find("chunked") !=
                   std::string::npos;
    int64_t content_length = response.header("content-length").empty()
                                 ? -1
                                 : (int64_t)to_double(response.header("content-length"), -1);

    std::string leftover = buffer.substr(header_end + 4);
    FILE* out_file = nullptr;
    if (!current.download_path.empty() && response.ok()) {
      fs::mkdir_p(fs::dirname(current.download_path));
      out_file = fopen((current.download_path + ".part").c_str(), "wbe");
      if (!out_file) {
        response.error = "cannot write " + current.download_path;
        return response;
      }
    }

    int64_t received = 0;
    auto consume = [&](const char* data, size_t size) -> bool {
      received += (int64_t)size;
      if (out_file) {
        return fwrite(data, 1, size, out_file) == size;
      }
      if (response.body.size() + size > kMaxInMemoryBody) return false;
      response.body.append(data, size);
      return true;
    };

    bool complete = false;
    if (chunked) {
      // Chunked decoding, with the leftover from the header read first.
      std::string pending = leftover;
      while (!complete) {
        size_t line_end = pending.find("\r\n");
        if (line_end == std::string::npos) {
          ssize_t n = socket.read_some(chunk, sizeof(chunk), current.timeout_ms);
          if (n <= 0) break;
          pending.append(chunk, (size_t)n);
          continue;
        }
        size_t chunk_size = (size_t)strtoul(pending.substr(0, line_end).c_str(), nullptr, 16);
        if (chunk_size == 0) {
          complete = true;
          break;
        }
        pending.erase(0, line_end + 2);
        while (pending.size() < chunk_size + 2) {
          ssize_t n = socket.read_some(chunk, sizeof(chunk), current.timeout_ms);
          if (n <= 0) break;
          pending.append(chunk, (size_t)n);
        }
        if (pending.size() < chunk_size) break;
        if (!consume(pending.data(), chunk_size)) {
          response.error = "the reply was too large";
          break;
        }
        pending.erase(0, std::min(pending.size(), chunk_size + 2));
        if (current.on_progress) current.on_progress(received, content_length);
      }
    } else {
      if (!leftover.empty() && !consume(leftover.data(), leftover.size())) {
        response.error = "the reply was too large";
      }
      while (response.error.empty()) {
        if (content_length >= 0 && received >= content_length) {
          complete = true;
          break;
        }
        ssize_t n = socket.read_some(chunk, sizeof(chunk), current.timeout_ms);
        if (n < 0) break;
        if (n == 0) {
          complete = content_length < 0 || received >= content_length;
          break;
        }
        if (!consume(chunk, (size_t)n)) {
          response.error = "the reply was too large";
          break;
        }
        if (current.on_progress) current.on_progress(received, content_length);
      }
    }

    if (out_file) {
      fclose(out_file);
      if (complete && response.error.empty()) {
        fs::rename(current.download_path + ".part", current.download_path);
      } else {
        fs::remove_file(current.download_path + ".part");
      }
    }
    if (!complete && response.error.empty()) {
      response.error = "the download was cut short";
    }
    return response;
  }

  response.error = "too many redirects";
  return response;
}

HttpResponse http_get(const std::string& url, int timeout_ms) {
  HttpRequest request;
  request.timeout_ms = timeout_ms;
  if (!Url::parse(url, request.url)) {
    HttpResponse response;
    response.error = "not a usable address: " + url;
    return response;
  }
  return http_perform(request);
}

HttpResponse http_post(const std::string& url, const std::string& content_type,
                       const std::string& body, int timeout_ms) {
  HttpRequest request;
  request.method = "POST";
  request.body = body;
  request.timeout_ms = timeout_ms;
  request.headers["Content-Type"] = content_type;
  if (!Url::parse(url, request.url)) {
    HttpResponse response;
    response.error = "not a usable address: " + url;
    return response;
  }
  return http_perform(request);
}

HttpResponse http_download(const std::string& url, const std::string& path,
                           std::function<void(int64_t, int64_t)> progress) {
  HttpRequest request;
  request.timeout_ms = 60000;
  request.download_path = path;
  request.on_progress = std::move(progress);
  if (!Url::parse(url, request.url)) {
    HttpResponse response;
    response.error = "not a usable address: " + url;
    return response;
  }
  return http_perform(request);
}

bool https_available() { return !find_tls_tool().empty(); }

std::string url_encode(const std::string& text) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

std::string url_decode(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '+') {
      out += ' ';
    } else if (text[i] == '%' && i + 2 < text.size()) {
      out += (char)strtol(text.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

std::string base64_encode(const std::string& data) {
  static const char* kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  while (i + 2 < data.size()) {
    uint32_t triple = ((uint32_t)(unsigned char)data[i] << 16) |
                      ((uint32_t)(unsigned char)data[i + 1] << 8) |
                      (uint32_t)(unsigned char)data[i + 2];
    out += kTable[(triple >> 18) & 0x3F];
    out += kTable[(triple >> 12) & 0x3F];
    out += kTable[(triple >> 6) & 0x3F];
    out += kTable[triple & 0x3F];
    i += 3;
  }
  if (i < data.size()) {
    uint32_t triple = (uint32_t)(unsigned char)data[i] << 16;
    bool two = i + 1 < data.size();
    if (two) triple |= (uint32_t)(unsigned char)data[i + 1] << 8;
    out += kTable[(triple >> 18) & 0x3F];
    out += kTable[(triple >> 12) & 0x3F];
    out += two ? kTable[(triple >> 6) & 0x3F] : '=';
    out += '=';
  }
  return out;
}

}  // namespace ck
