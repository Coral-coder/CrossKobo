#include "net/libgen.h"

#include <algorithm>

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/paths.h"
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
  // "https://host?req=..." with no slash before the query leaves the query
  // stuck on the host; peel it back off so the host stays a host.
  size_t q = url.host.find('?');
  if (q != std::string::npos) {
    if (url.query.empty()) url.query = url.host.substr(q + 1);
    url.host = url.host.substr(0, q);
  }
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

// A link seen inside a table row: where it points and the words it showed.
struct RowLink {
  std::string href;
  std::string text;
};

// Is this link text just a mirror marker - "[1]", "2", "libgen", "GET" -
// rather than a book title? libgen.li puts an md5 in every mirror link in
// the last column, and their text is a number or a host name, never the
// title. We must not mistake one of those for the title.
bool is_mirror_marker(const std::string& text) {
  std::string t = to_lower(trim(text));
  if (t.empty()) return true;
  // Strip surrounding brackets/parens a marker like "[1]" carries.
  while (!t.empty() && (t.front() == '[' || t.front() == '(')) t.erase(t.begin());
  while (!t.empty() && (t.back() == ']' || t.back() == ')')) t.pop_back();
  t = trim(t);
  if (t.empty()) return true;
  if (t.find_first_not_of("0123456789") == std::string::npos) return true;  // a number
  static const char* kMarkers[] = {"get",       "download", "mirror", "libgen",
                                    "library",   "annas",    "books",  "[1]",
                                    "[2]",       "[3]",      "[4]",    "[5]"};
  for (const char* marker : kMarkers) {
    if (t == marker) return true;
  }
  return false;
}

void parse_libgen_html(const std::string& html, std::vector<SearchResult>& out) {
  XmlParser parser(html);
  // Cells of the row being read, plus every link the row carried, with the
  // text each link showed - the title is one of them.
  std::vector<std::string> cells;
  std::vector<RowLink> links;
  std::string cell_text;
  std::string link_text;
  std::string link_href;
  std::string row_cover;   // the first <img> in the row: a cover thumbnail
  bool in_row = false;
  bool in_cell = false;
  bool in_link = false;

  auto finish_row = [&]() {
    if (!cells.empty() || !links.empty()) {
      SearchResult result;
      // Pick the row's md5 and its title together. Prefer the md5 link whose
      // text reads like a title (longest non-marker text); libgen.li's title
      // link carries the md5 on the fiction pages, while on the non-fiction
      // table the md5 sits in the numbered mirror links and the title link
      // has none - so fall back to any md5 in the row, and to the longest
      // cell for the title.
      const RowLink* title_link = nullptr;
      std::string any_md5;
      for (const RowLink& link : links) {
        std::string md5 = md5_from_link(link.href);
        if (md5.empty()) continue;
        if (any_md5.empty()) any_md5 = md5;
        if (is_mirror_marker(link.text)) continue;
        if (!title_link || trim(link.text).size() > trim(title_link->text).size()) {
          title_link = &link;
        }
      }
      result.md5 = title_link ? md5_from_link(title_link->href) : any_md5;
      if (result.md5.empty()) result.md5 = any_md5;

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
        // The title is the text of the md5 title link; if none looked like a
        // title (the non-fiction table), take the longest cell that is not a
        // year, size or extension.
        if (title_link) result.title = trim(title_link->text);
        if (result.title.empty() || is_mirror_marker(result.title)) {
          result.title.clear();
          for (const std::string& cell : cells) {
            std::string value = trim(cell);
            if (value.size() <= result.title.size()) continue;
            if (is_book_extension(value) || looks_like_size(value) || looks_like_year(value))
              continue;
            result.title = value;
          }
        }
        // The author is usually the cell just before the one holding the
        // title, and both beat anything guessed by shape.
        for (size_t i = 0; i < cells.size(); ++i) {
          if (result.title.empty()) break;
          if (trim(cells[i]).find(result.title) != std::string::npos && i > 0) {
            std::string previous = trim(cells[i - 1]);
            if (!previous.empty() && !looks_like_year(previous) &&
                previous.find_first_not_of("0123456789") != std::string::npos) {
              result.author = previous;
            }
            break;
          }
        }
        result.cover_url = trim(row_cover);
        out.push_back(result);
      }
    }
    cells.clear();
    links.clear();
    row_cover.clear();
    in_link = false;
  };

  while (true) {
    XmlParser::Token token = parser.next();
    if (token == XmlParser::Token::End) break;
    if (token == XmlParser::Token::Text) {
      if (in_cell) cell_text += parser.text();
      if (in_link) link_text += parser.text();
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
        link_href = parser.attr("href");
        link_text.clear();
        in_link = !link_href.empty();
      } else if (tag == "img" && in_row && row_cover.empty()) {
        row_cover = parser.attr("src");
      }
      continue;
    }
    // End tag.
    if (tag == "a" && in_link) {
      links.push_back({link_href, link_text});
      in_link = false;
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

namespace {
// Unescape the HTML entities that turn up inside an href, the way the
// reference downloader does: &amp; &gt; &lt;.
std::string unescape_href(std::string s) {
  auto replace_all_of = [&](const std::string& from, const std::string& to) {
    size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos) {
      s.replace(at, from.size(), to);
      at += to.size();
    }
  };
  replace_all_of("&amp;", "&");
  replace_all_of("&gt;", ">");
  replace_all_of("&lt;", "<");
  return s;
}
}  // namespace

std::string get_link_from_ads_page(const std::string& html, const std::string& base) {
  std::string lower = to_lower(html);
  if (lower.find("get.php") == std::string::npos) return "";
  std::string best;
  int best_score = -1;
  size_t pos = 0;
  // Walk every href="..." value and keep the get.php link, preferring one
  // that carries a key= token and whose visible label is GET - that is the
  // real, keyed download link the ads page puts the file behind.
  while (true) {
    size_t at = lower.find("href", pos);
    if (at == std::string::npos) break;
    size_t eq = lower.find('=', at);
    if (eq == std::string::npos) break;
    size_t q = eq + 1;
    while (q < html.size() && (html[q] == ' ' || html[q] == '\t')) ++q;
    if (q >= html.size() || (html[q] != '"' && html[q] != '\'')) {
      pos = eq + 1;
      continue;
    }
    size_t end = html.find(html[q], q + 1);
    if (end == std::string::npos) break;
    std::string href = html.substr(q + 1, end - q - 1);
    pos = end + 1;
    std::string hl = to_lower(href);
    if (hl.find("get.php") == std::string::npos) continue;

    int score = 0;
    if (hl.find("key=") != std::string::npos) score += 2;
    // Does the anchor's visible text say GET? (often <h2>GET</h2>)
    size_t gt = html.find('>', end);
    if (gt != std::string::npos) {
      std::string after = to_lower(html.substr(gt, std::min<size_t>(80, html.size() - gt)));
      if (after.find(">get<") != std::string::npos || after.find("get</") != std::string::npos) {
        score += 1;
      }
    }
    if (score > best_score) {
      std::string url = unescape_href(href);
      if (url.rfind("http", 0) != 0) url = resolve_url(base + "/", url);
      best = url;
      best_score = score;
    }
  }
  return best;
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
  std::vector<std::string> candidates;

  // What to ask. If the address you gave already names a search page - a real
  // path like /index.php - that page is used as THE address. If it names only
  // a host, or a host with a bare "?req=" query and no page, we probe the
  // pages Library-Genesis forks conventionally search at, because "/" itself
  // is only ever a front page, never search.
  //
  // Turn the query the address carried (with or without a {searchTerms}
  // placeholder, or a bare trailing "req=") into a finished query string.
  auto build_query = [&](const std::string& q) -> std::string {
    std::string filled = fill_placeholder(q, encoded);
    if (filled != q) return filled;              // had a placeholder
    if (!q.empty() && q.back() == '=') return q + encoded;  // "...?req=" -> append
    return q + "&q=" + encoded;
  };
  // The pages Library-Genesis forks and front-ends put search behind, most-
  // likely first. A fiction server (a fanfic fork is one) searches at
  // /fiction/?q=; the classic non-fiction table is /search.php?req=; newer
  // forks /index.php?req=; Anna's-Archive-style front-ends
  // /search?...&q=&display=table; some expose a JSON API at /json.php.
  // libgen.li / libgen.gl (and clones) answer "No Request keys" to a bare
  // req=term: their index.php search must also say which columns to match,
  // which object kinds to return and which topics to include. This is the
  // full search - fiction, non-fiction, comics, magazines - the way the site
  // itself issues it.
  const std::string li_keys =
      "&columns%5B%5D=t&columns%5B%5D=a&columns%5B%5D=s&columns%5B%5D=y"
      "&columns%5B%5D=p&columns%5B%5D=i"
      "&objects%5B%5D=f&objects%5B%5D=e&objects%5B%5D=s&objects%5B%5D=a"
      "&objects%5B%5D=p&objects%5B%5D=w"
      "&topics%5B%5D=l&topics%5B%5D=c&topics%5B%5D=f&topics%5B%5D=a"
      "&topics%5B%5D=m&topics%5B%5D=r&topics%5B%5D=s"
      "&res=100&filesuns=all";
  auto add_probes = [&](const std::string& q_or_empty) {
    std::string q = q_or_empty;                  // the param the user named, if any
    // The reference downloader's exact form first: index.php?req=...&res=N.
    candidates.push_back(endpoint.base + "/index.php?req=" + encoded + "&res=100");
    // Then the full-keys request, for clones that demand the column set.
    candidates.push_back(endpoint.base + "/index.php?req=" + encoded + li_keys);
    if (!q.empty()) {
      // We know the parameter name (e.g. "req="), just not the page. Try it
      // on each real search page before giving up.
      std::string qs = build_query(q);
      candidates.push_back(endpoint.base + "/index.php?" + qs);
      candidates.push_back(endpoint.base + "/search.php?" + qs);
      candidates.push_back(endpoint.base + "/fiction/?" + qs);
    }
    candidates.push_back(endpoint.base + "/fiction/?q=" + encoded);
    candidates.push_back(endpoint.base + "/search.php?req=" + encoded + "&column=def");
    candidates.push_back(endpoint.base + "/search.php?req=" + encoded);
    candidates.push_back(endpoint.base + "/index.php?req=" + encoded);
    candidates.push_back(endpoint.base +
                         "/search?index=&page=1&display=table&q=" + encoded);
    candidates.push_back(endpoint.base + "/search?q=" + encoded);
    candidates.push_back(endpoint.base + "/search?req=" + encoded);
    candidates.push_back(endpoint.base +
                         "/json.php?object=e&addkeys=*&fields=*&req=" + encoded);
  };

  if (!endpoint.path.empty()) {
    // A real path was given (e.g. /index.php): use it exactly as THE address.
    std::string url = endpoint.base + endpoint.path;
    if (!endpoint.query.empty()) {
      url += "?" + build_query(endpoint.query);
    } else {
      url += (endpoint.path.find("/fiction") != std::string::npos ? "?q=" : "?req=") + encoded;
    }
    candidates.push_back(url);
    // If that exact page turns up nothing, still try the conventional pages
    // on the same host - the path may have been the home page, not search.
    add_probes(endpoint.query);
  } else {
    // No path, only a host (with or without a "?req=" style query). Probe the
    // conventional pages so pointing at just the server, the way
    // calibrain/shelfmark does, works - hitting "/" itself would only ever
    // reach the front page, never search.
    add_probes(endpoint.query);
  }

  std::string last_error;
  std::string probe_log;   // one line per attempt, for diagnosis
  std::string best_body;   // the most useful answer to keep for inspection
  std::string best_url;
  int best_score = -1;
  // How informative is an unparsed answer? A real search page from index.php
  // beats a stray front page, which beats an error string.
  auto score = [](const std::string& url, const std::string& body) {
    int s = 0;
    if (url.find("index.php") != std::string::npos) s += 4;
    if (body.find("<table") != std::string::npos ||
        body.find("<tr") != std::string::npos) s += 3;
    if (body.find("Welcome to nginx") != std::string::npos) s -= 2;
    return s;
  };
  for (const std::string& url : candidates) {
    HttpResponse response = http_get(url, 20000);
    std::string snippet = trim(response.body).substr(0, 80);
    for (char& ch : snippet) {
      if (ch == '\n' || ch == '\r') ch = ' ';
    }
    if (!response.error.empty()) {
      last_error = response.error;
      probe_log += format("%s -> error: %s\n", url.c_str(), response.error.c_str());
      continue;
    }
    probe_log += format("%s -> %d, %zu bytes: %s\n", url.c_str(), response.status,
                        response.body.size(), snippet.c_str());
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
      // Cover thumbnails come through relative; make them absolute against
      // the server so the Kobo's browser can load them.
      for (SearchResult& r : results) {
        if (!r.cover_url.empty() && r.cover_url.rfind("http", 0) != 0 &&
            r.cover_url.rfind("data:", 0) != 0) {
          r.cover_url = resolve_url(endpoint.base + "/", r.cover_url);
        }
      }
      CK_LOGI("search: %s -> %zu results (html)", url.c_str(), results.size());
      return true;
    }
    int s = score(url, body);
    if (s > best_score) {
      best_score = s;
      best_body = body;
      best_url = url;
    }
    last_error = "nothing recognisable in the answer";
    CK_LOGI("search: %s answered %zu bytes with nothing to show", url.c_str(), body.size());
  }
  // Nothing parsed. Save the log of every page tried, plus the most useful
  // answer, so what the server actually does can be read rather than guessed.
  {
    std::string dump = paths().data + "/last-search.html";
    std::string contents = "<!-- Shelfmark search diagnostics\nquery: " + query +
                           "\nkept: " + best_url + "\n\n" + probe_log + "-->\n" + best_body;
    if (fs::write_file_atomic(dump, contents)) {
      CK_LOGI("search: saved diagnostics to %s", dump.c_str());
    }
  }
  error = last_error.empty() ? "The server did not answer." : last_error;
  return false;
}

bool libgen_resolve_download(const std::string& base_address, const SearchResult& result,
                             std::string& url_out, std::string& referer_out,
                             std::string& error) {
  referer_out.clear();
  if (!result.direct_url.empty()) {
    url_out = result.direct_url;
    return true;
  }
  Endpoint endpoint = split_endpoint(base_address);
  if (endpoint.base.empty() || result.md5.empty()) {
    error = "Nothing to download: the server gave neither a link nor a file id.";
    return false;
  }
  // The reference downloader's path: the ads.php page for the md5 carries a
  // keyed get.php link (the key is a per-page token, not derivable), and the
  // file GET must be sent with that ads page as its Referer. Do exactly that.
  std::string ads = endpoint.base + "/ads.php?md5=" + result.md5;
  HttpResponse ads_page = http_get(ads, 15000);
  if (ads_page.ok() && !ads_page.body.empty()) {
    std::string link = get_link_from_ads_page(ads_page.body, endpoint.base);
    if (!link.empty()) {
      url_out = link;
      referer_out = ads;
      return true;
    }
  }
  // Fallbacks for forks that are not libgen.li: other pages that link to the
  // file, scraped the same way, then any file-looking link on them.
  std::vector<std::string> pages = {
      endpoint.base + "/index.php?md5=" + result.md5,
      endpoint.base + "/file.php?md5=" + result.md5,
      endpoint.base + "/main/" + result.md5,
      endpoint.base + "/fiction/" + result.md5,
  };
  for (const std::string& url : pages) {
    HttpResponse response = http_get(url, 15000);
    if (!response.ok() || response.body.empty()) continue;
    std::string link = get_link_from_ads_page(response.body, endpoint.base);
    if (link.empty()) link = direct_link_from_page(response.body, endpoint.base);
    if (!link.empty()) {
      url_out = link;
      referer_out = url;
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
  std::string referer;
  if (!libgen_resolve_download(base, result, url, referer, error)) return false;

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
  // The file GET goes with the ads page as its Referer, the way the server
  // expects, or it hands back an error page instead of the book.
  if (!referer.empty()) request.headers["Referer"] = referer;
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
  // Anything below a few KB is an error or challenge page, not a book - the
  // reference downloader rejects the same way rather than saving junk.
  if (fs::file_size(target) < 10 * 1024) {
    fs::remove_file(target);
    error = "The server sent an error page instead of the file. Try again, or "
            "open the book's page in the browser to see what it wants.";
    return false;
  }
  path_out = target;
  CK_LOGI("search: downloaded %s (%llu bytes)", target.c_str(),
          (unsigned long long)fs::file_size(target));
  return true;
}

}  // namespace ck
