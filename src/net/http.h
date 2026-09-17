#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ck {

struct Url {
  std::string scheme = "http";
  std::string host;
  int port = 80;
  std::string path = "/";
  std::string query;
  std::string user;
  std::string password;

  static bool parse(const std::string& text, Url& out);
  std::string to_string() const;
  std::string request_target() const { return query.empty() ? path : path + "?" + query; }
};

struct HttpResponse {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;   // keys lowercased
  std::string error;

  bool ok() const { return status >= 200 && status < 300; }
  std::string header(const std::string& name) const;
};

struct HttpRequest {
  std::string method = "GET";
  Url url;
  std::map<std::string, std::string> headers;
  std::string body;
  int timeout_ms = 20000;
  int max_redirects = 5;
  // When set, the body is written here as it arrives instead of being kept
  // in memory - which is how books are downloaded.
  std::string download_path;
  // Called with (received, total); total is 0 when the server does not say.
  std::function<void(int64_t, int64_t)> on_progress;
};

// A small blocking HTTP/1.1 client: enough for OPDS catalogues, progress
// sync and downloading books, with no dependency on an HTTP library.
//
// HTTPS needs a TLS stack CrossKobo does not link. When an https:// URL is
// requested, the client looks for curl or wget on the device and uses
// whichever it finds; if neither is there it fails with an explanation
// rather than silently downgrading to plaintext.
HttpResponse http_perform(const HttpRequest& request);
HttpResponse http_get(const std::string& url, int timeout_ms = 20000);
HttpResponse http_post(const std::string& url, const std::string& content_type,
                       const std::string& body, int timeout_ms = 20000);
// Downloads to a file, creating parent directories. Returns the response
// with an empty body on success.
HttpResponse http_download(const std::string& url, const std::string& path,
                           std::function<void(int64_t, int64_t)> progress = nullptr);

// True when an https:// URL can be fetched on this device.
bool https_available();

std::string url_encode(const std::string& text);
std::string url_decode(const std::string& text);
std::string base64_encode(const std::string& data);

}  // namespace ck
