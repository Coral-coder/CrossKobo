#pragma once
#include <string>
#include <vector>

namespace ck {

// A client for the search format Library Genesis popularised, which a lot of
// self-hosted catalogue software has inherited by forking it: a search
// endpoint that answers with either a simple HTML table or a JSON array,
// and files addressed by their MD5.
//
// CrossKobo ships no addresses for this. It is a protocol adapter: you give
// it the base URL of a server you run, and it searches and downloads from
// that server. The parsing is deliberately forgiving, because every fork
// has moved the columns around.
struct SearchResult {
  std::string title;
  std::string author;
  std::string extension;   // lowercase, no dot
  std::string size_text;   // as the server wrote it, for display
  std::string year;
  std::string md5;
  std::string direct_url;  // set when the answer gave one outright
  std::string cover_url;   // a thumbnail, when the row carried one

  // A filename for the download, built from what the server said.
  std::string filename() const;
};

// Searches `base` (e.g. "http://192.168.1.10:8080") for `query`. Tries the
// JSON endpoint first, then the HTML one, and reports which it used in the
// log. Returns false with `error` set when neither answered usefully.
bool libgen_search(const std::string& base, const std::string& query,
                   std::vector<SearchResult>& results, std::string& error);

// Works out a URL the file can actually be fetched from, following the
// mirror pages these servers use when the search answer did not include
// one.
bool libgen_resolve_download(const std::string& base, const SearchResult& result,
                             std::string& url_out, std::string& referer_out,
                             std::string& error);

// Downloads a result into `dir`, never overwriting. Returns the path.
bool libgen_download(const std::string& base, const SearchResult& result,
                     const std::string& dir, const std::string& user,
                     const std::string& password, std::string& path_out,
                     std::string& error);

// --- Parsing, exposed so it can be tested without a server -----------------

// Parses the simple-view HTML table. Columns are found by what they contain
// rather than by position, since forks disagree about the order.
void parse_libgen_html(const std::string& html, std::vector<SearchResult>& out);
// Parses the JSON array form (an array of objects, keys in any case).
bool parse_libgen_json(const std::string& text, std::vector<SearchResult>& out);
// The MD5 in a link, or empty. Handles "md5=..." and "/main/<md5>" shapes.
std::string md5_from_link(const std::string& href);
// Picks the file link out of a mirror page.
std::string direct_link_from_page(const std::string& html, const std::string& base);
// Scrapes the keyed "get.php?md5=...&key=..." download link out of an
// ads.php page, the way the reference downloader does. Empty if none.
std::string get_link_from_ads_page(const std::string& html, const std::string& base);
// True when the address looks like one of these search endpoints.
bool looks_like_libgen_endpoint(const std::string& url);

}  // namespace ck
