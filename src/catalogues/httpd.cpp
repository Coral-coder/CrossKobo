#include "catalogues/httpd.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <vector>

#include "core/clock.h"
#include "core/log.h"
#include "core/str.h"

namespace catalogues {
namespace {

bool write_all(int fd, const char* data, size_t size) {
  while (size > 0) {
    ssize_t n = write(fd, data, size);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += n;
    size -= (size_t)n;
  }
  return true;
}

const char* reason_for(int status) {
  switch (status) {
    case 200: return "OK";
    case 302: return "Found";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    default: return "OK";
  }
}

void send(int fd, const Response& response) {
  std::string head = ck::format("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n",
                                response.status, reason_for(response.status),
                                response.content_type.c_str(), response.body.size());
  if (!response.location.empty()) head += "Location: " + response.location + "\r\n";
  head += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
  if (!write_all(fd, head.data(), head.size())) return;
  if (!response.body.empty()) write_all(fd, response.body.data(), response.body.size());
}

// Reads one request: the head up to the blank line, then as much body as
// Content-Length promises. The Kobo's browser is the only client, so a
// generous but finite limit is enough.
bool read_request(int fd, Request& out) {
  std::string data;
  char buffer[4096];
  size_t head_end = std::string::npos;
  while (head_end == std::string::npos) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 15000) <= 0) return false;
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n <= 0) return false;
    data.append(buffer, (size_t)n);
    head_end = data.find("\r\n\r\n");
    if (data.size() > 64 * 1024) return false;
  }
  std::string head = data.substr(0, head_end);
  std::string body = data.substr(head_end + 4);

  std::vector<std::string> lines = ck::split(head, '\n');
  if (lines.empty()) return false;
  std::vector<std::string> parts = ck::split(ck::trim(lines[0]), ' ');
  if (parts.size() < 2 || parts[1].empty() || parts[1][0] != '/') return false;
  out.method = parts[0];
  std::string target = parts[1];
  size_t q = target.find('?');
  out.path = url_decode(q == std::string::npos ? target : target.substr(0, q));
  if (q != std::string::npos) out.query = parse_form(target.substr(q + 1));
  for (size_t i = 1; i < lines.size(); ++i) {
    std::string line = ck::trim(lines[i]);
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    out.headers[ck::to_lower(ck::trim(line.substr(0, colon)))] =
        ck::trim(line.substr(colon + 1));
  }

  size_t length = (size_t)ck::to_int(out.headers["content-length"], 0);
  if (length > 1024 * 1024) return false;
  while (body.size() < length) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 15000) <= 0) return false;
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n <= 0) return false;
    body.append(buffer, (size_t)n);
  }
  out.body = body.substr(0, length);
  if (out.headers["content-type"].find("application/x-www-form-urlencoded") !=
      std::string::npos) {
    out.form = parse_form(out.body);
  }
  return true;
}

}  // namespace

std::string Request::param(const std::string& name) const {
  auto it = form.find(name);
  if (it != form.end()) return it->second;
  it = query.find(name);
  return it == query.end() ? "" : it->second;
}

std::string url_decode(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < text.size()) {
      char hex[3] = {text[i + 1], text[i + 2], 0};
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

std::map<std::string, std::string> parse_form(const std::string& text) {
  std::map<std::string, std::string> out;
  for (const std::string& pair : ck::split(text, '&')) {
    size_t eq = pair.find('=');
    std::string key = url_decode(eq == std::string::npos ? pair : pair.substr(0, eq));
    std::string value = eq == std::string::npos ? "" : url_decode(pair.substr(eq + 1));
    if (!key.empty()) out[key] = value;
  }
  return out;
}

std::string json_escape(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += ck::format("\\u%04x", c);
        } else {
          out += (char)c;
        }
    }
  }
  return out;
}

std::string html_escape(const std::string& text) {
  std::string out;
  for (char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

bool ping(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  bool ok = connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0;
  if (ok) {
    const char* request = "GET /status HTTP/1.0\r\n\r\n";
    ok = write_all(fd, request, strlen(request));
    char buffer[64];
    ssize_t n = ok ? read(fd, buffer, sizeof(buffer) - 1) : -1;
    ok = n > 0 && std::string(buffer, (size_t)n).find(" 200 ") != std::string::npos;
  }
  close(fd);
  return ok;
}

bool serve(const std::string& bind_address, int port, const Handler& handler,
           const volatile bool* stop, int idle_timeout_ms,
           const std::function<void()>& on_ready) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    CK_LOGE("httpd: socket: %s", strerror(errno));
    return false;
  }
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
    CK_LOGE("httpd: bad bind address %s", bind_address.c_str());
    close(listener);
    return false;
  }
  if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(listener, 8) < 0) {
    CK_LOGE("httpd: cannot listen on %s:%d: %s", bind_address.c_str(), port, strerror(errno));
    close(listener);
    return false;
  }
  CK_LOGI("httpd: listening on %s:%d", bind_address.c_str(), port);
  if (on_ready) on_ready();

  int64_t last_request = ck::now_ms();
  while (!(stop && *stop)) {
    if (idle_timeout_ms > 0 && ck::now_ms() - last_request > idle_timeout_ms) {
      CK_LOGI("httpd: idle for %d s, stopping", idle_timeout_ms / 1000);
      break;
    }
    struct pollfd pfd;
    pfd.fd = listener;
    pfd.events = POLLIN;
    int ready = poll(&pfd, 1, 500);
    if (ready <= 0) continue;
    int client = accept(listener, nullptr, nullptr);
    if (client < 0) continue;
    last_request = ck::now_ms();
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    Request request;
    if (read_request(client, request)) {
      send(client, handler(request));
    } else {
      Response bad;
      bad.status = 400;
      bad.content_type = "text/plain";
      bad.body = "bad request\n";
      send(client, bad);
    }
    close(client);
    last_request = ck::now_ms();
  }
  close(listener);
  return true;
}

}  // namespace catalogues
