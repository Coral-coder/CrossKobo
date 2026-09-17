#pragma once
#include <string>
#include <vector>

namespace ck {

// OPDS: the Atom-based catalogue format every self-hosted library speaks -
// Shelfmark, Calibre-Web, Kavita, Komga, BookLore, Project Gutenberg and
// the rest. A catalogue is a tree of feeds: navigation entries lead to more
// feeds, acquisition entries lead to a file to download.
struct OpdsEntry {
  std::string title;
  std::string author;
  std::string summary;
  std::string updated;
  // Where this entry leads. `download_url` is set on acquisition entries,
  // `feed_url` on navigation entries; an entry can carry both.
  std::string feed_url;
  std::string download_url;
  std::string download_type;   // MIME type of the acquisition link
  std::string cover_url;       // thumbnail, when the feed offers one
  int64_t size = 0;

  bool is_navigation() const { return download_url.empty() && !feed_url.empty(); }
  // A sensible filename for the downloaded book.
  std::string filename() const;
};

struct OpdsFeed {
  std::string title;
  std::string next_url;        // rel="next", for paged catalogues
  std::string search_url;      // OpenSearch template, with {searchTerms}
  std::string up_url;          // rel="up"
  std::vector<OpdsEntry> entries;
};

// Parses an OPDS (Atom) document. `base_url` resolves relative links.
// Tolerant by design: a feed that breaks one entry should still list the
// rest, because the alternative is a blank screen on a real server.
bool parse_opds(const std::string& xml, const std::string& base_url, OpdsFeed& out);

// Resolves a possibly relative URL against a base.
std::string resolve_url(const std::string& base, const std::string& href);

// Fills a search template from an OpenSearch description or a
// rel="search" link, substituting {searchTerms}.
std::string opds_search_url(const std::string& tmpl, const std::string& terms);

// Fetches and parses a feed. Adds Basic authentication when the catalogue
// has credentials. Returns false and sets `error` on failure.
bool fetch_opds(const std::string& url, const std::string& user, const std::string& password,
                OpdsFeed& out, std::string& error);

// Downloads an acquisition link into `dir`, returning the path written.
bool download_opds_entry(const OpdsEntry& entry, const std::string& dir,
                         const std::string& user, const std::string& password,
                         std::string& path_out, std::string& error);

}  // namespace ck
