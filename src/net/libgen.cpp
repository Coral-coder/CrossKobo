#include "net/libgen.h"

#include <algorithm>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/str.h"
#include <cstring>

#include "epub/xml.h"
#include "net/http.h"
#include "net/opds.h"   // resolve_url

namespace ck {
namespace {

// Extensions worth offering a reader. Anything else the server lists is
// still shown, but these are what the column sniffing recognises.
bool is_book_extension(const std::string& text) {
  static const char* kKnown[] = {"epub", "pdf",  "mobi", "azw3", "txt",
                                 "fb2",  "djvu", "cbz",  "cbr",  "html"};
  std::string lower = to_lower(trim(text));
  for (const char* known : kKnown) {
    if (lower == known) return true;
  }
  return false;
}

// "1.4 Mb", "755 Kb", "2 GB": a size in the HTML table always carries a
// unit, and insisting on one matters - a bare number is just as likely to
// be the id or the page count, which is exactly what it was read as before.
// Byte counts without units only turn up in the JSON form, where the field
// is named.
bool looks_like_size(const std::string& text) {
  std::string t = to_lower(trim(text));
  if (t.empty()) return false;
  bool has_digit = false;
  for (char ch : t) {
    if (ch >= '0' && ch <= '9') has_digit = true;
  }
  if (!has_digit) return false;
  return t.find("kb") != std::string::npos || t.find("mb") != std::string::npos ||
         t.find("gb") != std::string::npos || t.find("byte") != std::string::npos ||
         t.find(" b") != std::string::npos;
}

bool looks_like_year(const std::string& text) {
  std::string t = trim(text);
  if (t.size() != 4) return false;
  for (char ch : t) {
    if (ch < '0' || ch > '9') return false;
  }
  int year = to_int(t, 0);
  return year >= 1400 && year <= 2200;
}

// Case-insensitive member lookup: forks disagree about capitalisation.
std::string json_field(const Json& object, const std::vector<std::string>& names) {
  for (const auto& member : object.members()) {
    std::string key = to_lower(member.first);
    for (const std::string& name : names) {
      if (key == name) {
        const Json& value = member.second;
        if (value.is_string()) return value.as_string();
        if (value.is_number()) return format("%lld", (long long)value.as_int64());
      }
    }
  }
  return "";
}

// Splits a stored address into (base, endpoint-with-query) so a user can
// paste either "http://host:8080" or the search URL their server uses.
struct Endpoint {
  std::string base;      // scheme://host[:port], no trailing slash
  std::string path;      // the path of the pasted URL, "" when it was bare
  std::string query;     // its query, with the placeholder still in it
};

Endpoint split_endpoint(const std::string& address) {
  Endpoint out;
  Url url;
  if (!Url::parse(address, url)) return out;
  out.base = url.scheme + "://" + url.host;
  if (url.port > 0 && !((url.scheme == "http" && url.port == 80) ||
                        (url.scheme == "https" && url.port == 443))) {
    out.base += format(":%d", url.port);
  }
  if (url.path != "/") out.path = url.path;
  out.query = url.query;
  return out;
}

std::string fill_placeholder(const std::string& text, const std::string& query) {
  std::string out = text;
  const char* kTokens[] = {"{searchTerms}", "{searchterms}", "{query}", "{q}"};
  for (const char* token : kTokens) {
    size_t at = out.find(token);
    while (at != std::string::npos) {
      out.replace(at, strlen(token), query);
      at = out.find(token);
    }
  }
  return out;
}

}  // namespace

std::string SearchResult::filename() const {
  std::string base = title.empty() ? "download" : title;
  if (!author.empty()) base = author + " - " + base;
  base = fs::sanitize_filename(base);
  std::string ext = extension.empty() ? "epub" : to_lower(extension);
  return base + "." + ext;
}

bool looks_like_libgen_endpoint(const std::string& url) {
  std::string lower = to_lower(url);
  return lower.find("search.php") != std::string::npos ||
         lower.find("json.php") != std::string::npos ||
         lower.find("/index.php?req=") != std::string::npos;
}

std::string md5_from_link(const std::string& href) {
  std::string lower = to_lower(href);
  auto looks_like_md5 = [](const std::string& text) {
    if (text.size() != 32) return false;
    for (char ch : text) {
      bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      if (!hex) return false;
    }
    return true;
  };
  size_t at = lower.find("md5=");
  if (at != std::string::npos) {
    std::string tail = lower.substr(at + 4);
    size_t end = tail.find_first_of("&#\"' ");
    if (end != std::string::npos) tail = tail.substr(0, end);
    if (looks_like_md5(tail)) return tail;
  }
  // ".../main/<md5>" and ".../file/<md5>" shapes.
  size_t slash = lower.find_last_of('/');
  if (slash != std::string::npos) {
    std::string tail = lower.substr(slash + 1);
    size_t end = tail.find_first_of("?#");
    if (end != std::string::npos) tail = tail.substr(0, end);
    if (looks_like_md5(tail)) return tail;
  }
  return "";
}

void parse_libgen_html(const std::string& html, std::vector<SearchResult>& out) {
  XmlParser parser(html);
  // Cells of the row being read, plus every link the row carried.
  std::vector<std::string> cells;
  std::vector<std::string> hrefs;
  std::string cell_text;
  std::string title_from_link;
  bool in_row = false;
  bool in_cell = false;
  // Set while inside the link that carries the md5: its text is the title.
  bool in_title_link = false;

  auto finish_row = [&]() {
    if (!cells.empty()) {
      SearchResult result;
      for (const std::string& href : hrefs) {
        std::string md5 = md5_from_link(href);
        if (!md5.empty()) {
          result.md5 = md5;
          break;
        }
      }
      if (!result.md5.empty()) {
        // Identify columns by what they hold: forks move them around, but a
        // size still looks like a size and an extension like an extension.
        for (const std::string& cell : cells) {
          std::string value = trim(cell);
          if (value.empty()) continue;
          if (result.extension.empty() && is_book_extension(value)) {
            result.extension = to_lower(value);
          } else if (result.size_text.empty() && looks_like_size(value)) {
            result.size_text = value;
          } else if (result.year.empty() && looks_like_year(value)) {
            result.year = value;
          }
        }
        // The title is the cell the file link sat in; the author is usually
        // the cell before it, and both beat anything guessed by shape.
        result.title = trim(title_from_link);
        if (result.title.empty()) {
          for (const std::string& cell : cells) {
            if (trim(cell).size() > result.title.size()) result.title = trim(cell);
          }
        }
        for (size_t i = 0; i < cells.size(); ++i) {
          if (trim(cells[i]) == result.title && i > 0) {
            std::string previous = trim(cells[i - 1]);
            // Skip a leading numeric id column.
            if (!previous.empty() && !looks_like_year(previous) &&
                previous.find_first_not_of("0123456789") != std::string::npos) {
              result.author = previous;
            }
            break;
          }
        }
        out.push_back(result);
      }
    }
    cells.clear();
    hrefs.clear();
    title_from_link.clear();
  };

  while (true) {
    XmlParser::Token token = parser.next();
    if (token == XmlParser::Token::End) break;
    if (token == XmlParser::Token::Text) {
      if (in_cell) cell_text += parser.text();
      continue;
    }
    if (token != XmlParser::Token::StartTag && token != XmlParser::Token::EndTag) continue;
    std::string tag = to_lower(parser.tag());
    if (token == XmlParser::Token::StartTag) {
      if (tag == "tr") {
        finish_row();
        in_row = true;
      } else if (tag == "td" || tag == "th") {
        in_cell = true;
        cell_text.clear();
      } else if (tag == "a" && in_row) {
        std::string href = parser.attr("href");
        if (!href.empty()) {
          hrefs.push_back(href);
          // Remember the text of the link that carries an md5: that is the
          // title on every fork seen so far.
          if (!md5_from_link(href).empty()) in_title_link = true;
        }
      }
      continue;
    }
    // End tag.
    if (tag == "a" && in_title_link) {
      in_title_link = false;
      if (title_from_link.empty()) title_from_link = trim(cell_text);
    } else if (tag == "td" || tag == "th") {
      cells.push_back(cell_text);
      cell_text.clear();
      in_cell = false;
    } else if (tag == "tr") {
      finish_row();
      in_row = false;
    }
  }
  finish_row();
}

bool parse_libgen_json(const std::string& text, std::vector<SearchResult>& out) {
  Json json;
  if (!Json::parse(text, json)) return false;
  const Json* array = &json;
  // Some forks wrap the array in an object.
  if (json.is_object()) {
    for (const char* key : {"data", "results", "books"}) {
      if (const Json* found = json.find(key)) {
        if (found->is_array()) {
          array = found;
          break;
        }
      }
    }
  }
  if (!array->is_array()) return false;
  for (const Json& item : array->items()) {
    if (!item.is_object()) continue;
    SearchResult result;
    result.title = json_field(item, {"title", "name"});
    result.author = json_field(item, {"author", "authors", "creator"});
    result.md5 = to_lower(json_field(item, {"md5", "hash"}));
    result.extension = to_lower(json_field(item, {"extension", "ext", "format"}));
    result.year = json_field(item, {"year", "date"});
    std::string size = json_field(item, {"filesize", "size", "bytes"});
    if (!size.empty()) {
      // A bare byte count reads better as a human size.
      bool numeric = size.find_first_not_of("0123456789") == std::string::npos;
      result.size_text = numeric ? human_size((uint64_t)to_int(size, 0)) : size;
    }
    result.direct_url = json_field(item, {"download", "url", "link", "direct_url"});
    if (result.title.empty() && result.md5.empty() && result.direct_url.empty()) continue;
    out.push_back(result);
  }
  return true;
}

std::string direct_link_from_page(const std::string& html, const std::string& base) {
  XmlParser parser(html);
  std::string best;
  std::string current_href;
  std::string text;
  bool in_link = false;
  while (true) {
    XmlParser::Token token = parser.next();
    if (token == XmlParser::Token::End) break;
    if (token == XmlParser::Token::Text) {
      if (in_link) text += parser.text();
      continue;
    }
    if (token == XmlParser::Token::StartTag && to_lower(parser.tag()) == "a") {
      current_href = parser.attr("href");
      text.clear();
      in_link = true;
      continue;
    }
    if (token == XmlParser::Token::EndTag && to_lower(parser.tag()) == "a" && in_link) {
      in_link = false;
      std::string label = to_lower(trim(text));
      std::string href = current_href;
      if (href.empty()) continue;
      std::string lower = to_lower(href);
      bool looks_like_file = lower.find("get.php") != std::string::npos ||
                             lower.find("/get/") != std::string::npos ||
                             lower.find(".epub") != std::string::npos ||
                             lower.find(".pdf") != std::string::npos ||
                             lower.find(".mobi") != std::string::npos ||
                             lower.find(".azw3") != std::string::npos ||
                             lower.find(".txt") != std::string::npos ||
                             lower.find(".fb2") != std::string::npos ||
                             lower.find(".cbz") != std::string::npos;
      // A link labelled GET is the one these pages put the file behind.
      if (label == "get" || label == "download") return resolve_url(base + "/", href);
      if (looks_like_file && best.empty()) best = resolve_url(base + "/", href);
    }
  }
  return best;
}

bool libgen_search(const std::string& base_address, const std::string& query,
                   std::vector<SearchResult>& results, std::string& error) {
  results.clear();
  Endpoint endpoint = split_endpoint(base_address);
  if (endpoint.base.empty()) {
    error = "That does not look like a web address.";
    return false;
  }
  std::string encoded = url_encode(query);

  // What to ask. If the address you gave already names a search route -
  // any path, any query, with or without a {searchTerms} placeholder - that
  // is THE address, used exactly as given. No assumptions, no probing:
  // whatever your server answers there is parsed as JSON or as an HTML
  // table. Only a bare host with nothing after it makes us fall back to
  // probing the endpoints Library-Genesis forks conventionally use.
  std::vector<std::string> candidates;
  bool gave_route = !endpoint.path.empty() || !endpoint.query.empty();
  if (gave_route) {
    std::string url = endpoint.base + (endpoint.path.empty() ? "/" : endpoint.path);
    if (!endpoint.query.empty()) {
      std::string filled = fill_placeholder(endpoint.query, encoded);
      if (filled != endpoint.query) {
        url += "?" + filled;                         // had a placeholder
      } else if (!endpoint.query.empty() && endpoint.query.back() == '=') {
        url += "?" + endpoint.query + encoded;        // "...?req=" -> append term
      } else {
        url += "?" + endpoint.query + "&q=" + encoded;
      }
    } else {
      // A path but no query: libgen fiction uses q, the rest use req.
      url += (endpoint.path.find("/fiction") != std::string::npos ? "?q=" : "?req=") + encoded;
    }
    candidates.push_back(url);
  } else {
    // A bare host: probe the endpoints real forks answer, most-likely first.
    // A fiction server (a fanfic fork is one) searches at /fiction/?q=; the
    // classic non-fiction table is /search.php?req=; newer forks use
    // /index.php?req=; some expose a JSON API at /json.php.
    candidates.push_back(endpoint.base + "/fiction/?q=" + encoded);
    candidates.push_back(endpoint.base + "/search.php?req=" + encoded + "&column=def");
    candidates.push_back(endpoint.base + "/search.php?req=" + encoded);
    candidates.push_back(endpoint.base + "/index.php?req=" + encoded);
    candidates.push_back(endpoint.base +
                         "/json.php?object=e&addkeys=*&fields=*&req=" + encoded);
  }

  std::string last_error;
  for (const std::string& url : candidates) {
    HttpResponse response = http_get(url, 20000);
    if (!response.error.empty()) {
      last_error = response.error;
      continue;
    }
    if (!response.ok()) {
      last_error = format("%s answered %d", url.c_str(), response.status);
      continue;
    }
    std::string body = trim(response.body);
    if (body.empty()) {
      last_error = "an empty answer";
      continue;
    }
    if (body[0] == '[' || body[0] == '{') {
      if (parse_libgen_json(body, results) && !results.empty()) {
        CK_LOGI("search: %s -> %zu results (json)", url.c_str(), results.size());
        return true;
      }
    }
    parse_libgen_html(body, results);
    if (!results.empty()) {
      CK_LOGI("search: %s -> %zu results (html)", url.c_str(), results.size());
      return true;
    }
    last_error = "nothing recognisable in the answer";
    CK_LOGI("search: %s answered %zu bytes with nothing to show", url.c_str(), body.size());
  }
  error = last_error.empty() ? "The server did not answer." : last_error;
  return false;
}

bool libgen_resolve_download(const std::string& base_address, const SearchResult& result,
                             std::string& url_out, std::string& error) {
  if (!result.direct_url.empty()) {
    url_out = result.direct_url;
    return true;
  }
  Endpoint endpoint = split_endpoint(base_address);
  if (endpoint.base.empty() || result.md5.empty()) {
    error = "Nothing to download: the server gave neither a link nor a file id.";
    return false;
  }
  // The file first, then the pages that link to it.
  std::vector<std::string> direct = {
      endpoint.base + "/get.php?md5=" + result.md5,
      endpoint.base + "/get/" + result.md5,
  };
  for (const std::string& url : direct) {
    HttpRequest request;
    if (!Url::parse(url, request.url)) continue;
    request.method = "HEAD";
    request.timeout_ms = 10000;
    HttpResponse response = http_perform(request);
    if (response.ok()) {
      url_out = url;
      return true;
    }
  }
  std::vector<std::string> pages = {
      endpoint.base + "/fiction/" + result.md5,
      endpoint.base + "/main/" + result.md5,
      endpoint.base + "/file.php?md5=" + result.md5,
      endpoint.base + "/ads.php?md5=" + result.md5,
  };
  for (const std::string& url : pages) {
    HttpResponse response = http_get(url, 15000);
    if (!response.ok() || response.body.empty()) continue;
    std::string link = direct_link_from_page(response.body, endpoint.base);
    if (!link.empty()) {
      url_out = link;
      return true;
    }
  }
  error = "Could not find a download link for that result.";
  return false;
}

bool libgen_download(const std::string& base, const SearchResult& result,
                     const std::string& dir, const std::string& user,
                     const std::string& password, std::string& path_out,
                     std::string& error) {
  std::string url;
  if (!libgen_resolve_download(base, result, url, error)) return false;

  fs::mkdir_p(dir);
  std::string target = dir + "/" + result.filename();
  if (fs::exists(target)) {
    std::string stem = target;
    std::string ext;
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) {
      ext = stem.substr(dot);
      stem = stem.substr(0, dot);
    }
    for (int i = 2; i < 100 && fs::exists(target); ++i) {
      target = format("%s (%d)%s", stem.c_str(), i, ext.c_str());
    }
  }

  HttpRequest request;
  if (!Url::parse(url, request.url)) {
    error = "That download address is not valid.";
    return false;
  }
  request.download_path = target;
  request.timeout_ms = 180000;
  if (!user.empty()) {
    request.headers["Authorization"] = "Basic " + base64_encode(user + ":" + password);
  }
  HttpResponse response = http_perform(request);
  if (!response.error.empty() || !response.ok()) {
    fs::remove_file(target);
    error = response.error.empty() ? format("Download failed (%d).", response.status)
                                   : response.error;
    return false;
  }
  if (fs::file_size(target) == 0) {
    fs::remove_file(target);
    error = "The server sent an empty file.";
    return false;
  }
  path_out = target;
  CK_LOGI("search: downloaded %s (%llu bytes)", target.c_str(),
          (unsigned long long)fs::file_size(target));
  return true;
}

}  // namespace ck
