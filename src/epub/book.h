#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "epub/css.h"
#include "epub/zip.h"
#include "gfx/canvas.h"

namespace ck {

struct SpineItem {
  std::string id;
  std::string href;      // path inside the archive
  std::string media_type;
  bool linear = true;
};

struct TocEntry {
  std::string title;
  std::string href;      // may include a #fragment
  int depth = 0;
  int spine_index = -1;  // resolved where possible
};

struct BookMetadata {
  std::string title;
  std::string author;
  std::string language;
  std::string publisher;
  std::string description;
  std::string series;
  int series_index = 0;
  std::string identifier;
};

// An open book. Supports EPUB 2 and 3 (plus plain text and single-image
// archives through the same interface), reading resources straight out of
// the archive on demand rather than unpacking to disk.
class Book {
 public:
  enum class Format { Unknown, Epub, Text, Cbz };

  bool open(const std::string& path);
  void close();
  bool is_open() const { return format_ != Format::Unknown; }

  Format format() const { return format_; }
  const std::string& path() const { return path_; }
  const BookMetadata& metadata() const { return meta_; }
  const std::vector<SpineItem>& spine() const { return spine_; }
  const std::vector<TocEntry>& toc() const { return toc_; }
  const Stylesheet& stylesheet() const { return css_; }

  // Chapter text, already decoded. For text books the whole file is one
  // chapter split into manageable sections.
  bool read_chapter(int spine_index, std::string& out) const;
  // Resolves an href relative to the chapter it appeared in, then reads it.
  bool read_resource(const std::string& href, const std::string& base_href,
                     std::string& out) const;
  std::string resolve_href(const std::string& href, const std::string& base_href) const;

  // Cover art, decoded and cached. Returns nullptr when the book has none.
  const Canvas* cover() const;
  bool has_cover() const;

  // Index into the spine for a TOC href (ignoring any fragment).
  int spine_index_for_href(const std::string& href) const;
  // Anchor lookup: where in a chapter a "#fragment" points. Returns -1 when
  // unknown; the layout engine resolves anchors during pagination.
  std::string fragment_of(const std::string& href) const;

  // Uncompressed size of a spine item, used to weight progress across
  // chapters before they have been paginated.
  uint64_t spine_size(int index) const;

  // A stable identity for this book, used for per-book state and caches.
  std::string cache_key() const;

 private:
  bool open_epub();
  bool open_text();
  bool open_cbz();
  bool parse_container(std::string& opf_path);
  bool parse_opf(const std::string& opf_path);
  void parse_nav_document(const std::string& href);
  void parse_ncx(const std::string& href);
  void load_stylesheets();

  Format format_ = Format::Unknown;
  std::string path_;
  mutable ZipReader zip_;
  BookMetadata meta_;
  std::vector<SpineItem> spine_;
  std::vector<TocEntry> toc_;
  std::map<std::string, std::string> manifest_;       // id -> href
  std::map<std::string, std::string> manifest_types_;  // id -> media-type
  std::string opf_dir_;
  std::string cover_href_;
  Stylesheet css_;

  // Text books are chopped into sections so pagination stays responsive.
  std::vector<std::pair<size_t, size_t>> text_sections_;
  std::string text_body_;

  mutable std::unique_ptr<Canvas> cover_cache_;
  mutable bool cover_attempted_ = false;
};

}  // namespace ck
