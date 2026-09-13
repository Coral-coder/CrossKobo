#pragma once
#include <functional>
#include <map>
#include <string>

namespace catalogues {

// The smallest HTTP server that will do: one connection at a time, on the
// loopback address, serving the Kobo's own browser. Nothing here is reachable
// from the network, so there is no key, no auth and no rate limit - the only
// client is the device itself.
struct Request {
  std::string method;
  std::string path;                            // without the query
  std::map<std::string, std::string> query;    // decoded
  std::map<std::string, std::string> form;     // decoded, from a posted form
  std::map<std::string, std::string> headers;  // keys lowercased
  std::string body;

  // A value from the form, or failing that the query.
  std::string param(const std::string& name) const;
};

struct Response {
  int status = 200;
  std::string content_type = "text/html; charset=utf-8";
  std::string location;   // set for a redirect
  std::string body;
};

using Handler = std::function<Response(const Request&)>;

// Binds `bind_address`:`port` and serves until `stop` becomes true (checked
// between connections; may be null) or, when `idle_timeout_ms` is positive,
// after that long without a request. Returns false only when the socket
// could not be opened. `on_ready` is called once listening.
bool serve(const std::string& bind_address, int port, const Handler& handler,
           const volatile bool* stop = nullptr, int idle_timeout_ms = 0,
           const std::function<void()>& on_ready = nullptr);

// True when something answers on `port` of the loopback address.
bool ping(int port);

// Helpers shared with the page.
std::string json_escape(const std::string& text);
std::string html_escape(const std::string& text);
std::string url_decode(const std::string& text);
std::map<std::string, std::string> parse_form(const std::string& text);

}  // namespace catalogues
