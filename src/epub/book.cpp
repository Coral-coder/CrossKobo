#include "epub/book.h"

#include <algorithm>
#include <cstdio>

#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "epub/xml.h"

namespace ck {
namespace {

// Largest plain-text section we will lay out in one go. Big enough that
// chapter boundaries feel natural, small enough that pagination of a 5 MB
// text file stays instant.
constexpr size_t kTextSectionBytes = 48 * 1024;

std::string parent_dir(const std::string& href) {
  size_t slash = href.find_last_of('/');
  return slash == std::string::npos ? "" : href.substr(0, slash);
}

}  // namespace

bool Book::open(const std::string& path) {
  close();
  path_ = path;
  std::string ext = fs::extension(path);
  if (ext == "epub") {
    if (open_epub()) return true;
    close();
    return false;
  }
  if (ext == "cbz") {
    if (open_cbz()) return true;
    close();
    return false;
  }
  if (ext == "txt" || ext == "md" || ext == "text" || ext == "log") {
    if (open_text()) return true;
    close();
    return false;
  }
  // Unknown extension: try EPUB (some books arrive misnamed), then text.
  if (open_epub()) return true;
  close();
  if (open_text()) return true;
  close();
  return false;
}

void Book::close() {
  zip_.close();
  format_ = Format::Unknown;
  meta_ = BookMetadata();
  spine_.clear();
  toc_.clear();
  manifest_.clear();
  manifest_types_.clear();
  opf_dir_.clear();
  cover_href_.clear();
  css_.clear();
  text_sections_.clear();
  text_body_.clear();
  cover_cache_.reset();
  cover_attempted_ = false;
}

bool Book::open_epub() {
  if (!zip_.open(path_)) return false;
  std::string opf_path;
  if (!parse_container(opf_path)) {
    CK_LOGW("epub: no OPF found in %s", path_.c_str());
    return false;
  }
  if (!parse_opf(opf_path)) return false;
  if (spine_.empty()) {
    CK_LOGW("epub: empty spine in %s", path_.c_str());
    return false;
  }
  load_stylesheets();
  format_ = Format::Epub;
  if (meta_.title.empty()) meta_.title = fs::stem(path_);
  CK_LOGI("epub: \"%s\" by %s, %zu sections, %zu toc entries", meta_.title.c_str(),
          meta_.author.empty() ? "unknown" : meta_.author.c_str(), spine_.size(), toc_.size());
  return true;
}

bool Book::parse_container(std::string& opf_path) {
  std::string container;
  if (zip_.read("META-INF/container.xml", container)) {
    XmlParser p(container);
    XmlParser::Token t;
    while ((t = p.next()) != XmlParser::Token::End) {
      if (t == XmlParser::Token::StartTag && p.tag() == "rootfile") {
        std::string full = p.attr("full-path");
        if (!full.empty()) {
          opf_path = full;
          return true;
        }
      }
    }
  }
  // Some malformed books have no container.xml; look for any .opf.
  for (const std::string& e : zip_.entries()) {
    if (fs::extension(e) == "opf") {
      opf_path = e;
      return true;
    }
  }
  return false;
}

bool Book::parse_opf(const std::string& opf_path) {
  std::string opf;
  if (!zip_.read(opf_path, opf)) return false;
  opf_dir_ = parent_dir(opf_path);

  enum class In { None, Metadata, Manifest, Spine };
  In section = In::None;
  std::string meta_tag;
  std::string meta_text;
  std::string ncx_id;
  std::string nav_href;
  std::string pending_property;  // EPUB3 <meta property="...">

  XmlParser p(opf);
  XmlParser::Token t;
  while ((t = p.next()) != XmlParser::Token::End) {
    if (t == XmlParser::Token::StartTag) {
      std::string tag = p.tag();
      // Strip a namespace prefix: dc:title -> title.
      size_t colon = tag.find(':');
      std::string local = colon == std::string::npos ? tag : tag.substr(colon + 1);

      if (local == "metadata") {
        section = In::Metadata;
      } else if (local == "manifest") {
        section = In::Manifest;
      } else if (local == "spine") {
        section = In::Spine;
        ncx_id = p.attr("toc");
      } else if (section == In::Metadata) {
        meta_tag = local;
        meta_text.clear();
        if (local == "meta") {
          std::string name = to_lower(p.attr("name"));
          std::string content = p.attr("content");
          if (name == "cover" && !content.empty()) cover_href_ = "id:" + content;
          if (name == "calibre:series") meta_.series = content;
          if (name == "calibre:series_index") meta_.series_index = to_int(content);
          pending_property = to_lower(p.attr("property"));
        }
      } else if (section == In::Manifest && local == "item") {
        std::string id = p.attr("id");
        std::string href = p.attr("href");
        std::string type = p.attr("media-type");
        std::string props = to_lower(p.attr("properties"));
        if (!id.empty() && !href.empty()) {
          manifest_[id] = href;
          manifest_types_[id] = type;
          if (props.find("cover-image") != std::string::npos) cover_href_ = href;
          if (props.find("nav") != std::string::npos) nav_href = href;
        }
      } else if (section == In::Spine && local == "itemref") {
        std::string idref = p.attr("idref");
        auto it = manifest_.find(idref);
        if (it == manifest_.end()) continue;
        SpineItem item;
        item.id = idref;
        item.href = it->second;
        item.media_type = manifest_types_.count(idref) ? manifest_types_[idref] : "";
        item.linear = to_lower(p.attr("linear")) != "no";
        spine_.push_back(std::move(item));
      }
      if (p.self_closing()) meta_tag.clear();
      continue;
    }
    if (t == XmlParser::Token::Text && section == In::Metadata && !meta_tag.empty()) {
      meta_text += p.text();
      continue;
    }
    if (t == XmlParser::Token::EndTag) {
      std::string tag = p.tag();
      size_t colon = tag.find(':');
      std::string local = colon == std::string::npos ? tag : tag.substr(colon + 1);
      if (local == "metadata" || local == "manifest" || local == "spine") {
        section = In::None;
        continue;
      }
      if (section == In::Metadata && local == meta_tag) {
        std::string value = trim(meta_text);
        if (!value.empty()) {
          if (local == "title" && meta_.title.empty()) {
            meta_.title = value;
          } else if (local == "creator" && meta_.author.empty()) {
            meta_.author = value;
          } else if (local == "language" && meta_.language.empty()) {
            meta_.language = value;
          } else if (local == "publisher") {
            meta_.publisher = value;
          } else if (local == "description" && meta_.description.empty()) {
            meta_.description = value;
          } else if (local == "identifier" && meta_.identifier.empty()) {
            meta_.identifier = value;
          } else if (local == "meta") {
            // EPUB 3 refines series information this way.
            if (pending_property == "belongs-to-collection") meta_.series = value;
            if (pending_property == "group-position") meta_.series_index = to_int(value);
          }
        }
        meta_tag.clear();
        meta_text.clear();
      }
    }
  }

  // Cover declared by manifest id.
  if (starts_with(cover_href_, "id:")) {
    std::string id = cover_href_.substr(3);
    auto it = manifest_.find(id);
    cover_href_ = it == manifest_.end() ? "" : it->second;
  }
  // Table of contents: prefer the EPUB 3 nav document, fall back to NCX.
  if (!nav_href.empty()) parse_nav_document(nav_href);
  if (toc_.empty() && !ncx_id.empty()) {
    auto it = manifest_.find(ncx_id);
    if (it != manifest_.end()) parse_ncx(it->second);
  }
  if (toc_.empty()) {
    for (const auto& kv : manifest_types_) {
      if (kv.second == "application/x-dtbncx+xml") {
        parse_ncx(manifest_[kv.first]);
        break;
      }
    }
  }
  for (TocEntry& e : toc_) e.spine_index = spine_index_for_href(e.href);
  return true;
}

void Book::parse_nav_document(const std::string& href) {
  std::string doc;
  if (!zip_.read(fs::normalize(fs::join_path(opf_dir_, href)), doc)) return;
  std::string base = parent_dir(href);

  // Find the <nav epub:type="toc"> (or the first <nav>) and walk its list.
  XmlParser p(doc);
  XmlParser::Token t;
  bool in_toc = false;
  int depth = 0;
  std::string pending_href;
  std::string pending_text;
  bool in_anchor = false;
  while ((t = p.next()) != XmlParser::Token::End) {
    if (t == XmlParser::Token::StartTag) {
      const std::string& tag = p.tag();
      if (tag == "nav") {
        std::string type = to_lower(p.attr("epub:type") + p.attr("type") + p.attr("role"));
        in_toc = type.empty() || type.find("toc") != std::string::npos;
        depth = 0;
        continue;
      }
      if (!in_toc) continue;
      if (tag == "ol" || tag == "ul") {
        ++depth;
      } else if (tag == "a") {
        pending_href = p.attr("href");
        pending_text.clear();
        in_anchor = true;
      }
      continue;
    }
    if (t == XmlParser::Token::Text && in_anchor) {
      pending_text += p.text();
      continue;
    }
    if (t == XmlParser::Token::EndTag) {
      const std::string& tag = p.tag();
      if (tag == "nav") {
        if (in_toc && !toc_.empty()) break;
        in_toc = false;
      } else if (in_toc && (tag == "ol" || tag == "ul")) {
        depth = std::max(0, depth - 1);
      } else if (in_toc && tag == "a" && in_anchor) {
        in_anchor = false;
        std::string title = trim(pending_text);
        if (!title.empty() && !pending_href.empty()) {
          TocEntry e;
          e.title = title;
          e.href = fs::normalize(fs::join_path(base, pending_href));
          e.depth = std::max(0, depth - 1);
          toc_.push_back(std::move(e));
        }
      }
    }
  }
}

void Book::parse_ncx(const std::string& href) {
  std::string doc;
  if (!zip_.read(fs::normalize(fs::join_path(opf_dir_, href)), doc)) return;
  std::string base = parent_dir(href);

  XmlParser p(doc);
  XmlParser::Token t;
  int depth = -1;
  bool in_label_text = false;
  std::string label;
  std::string content;
  while ((t = p.next()) != XmlParser::Token::End) {
    if (t == XmlParser::Token::StartTag) {
      const std::string& tag = p.tag();
      if (tag == "navpoint") {
        ++depth;
        label.clear();
        content.clear();
      } else if (tag == "text") {
        in_label_text = true;
      } else if (tag == "content") {
        content = p.attr("src");
      }
      continue;
    }
    if (t == XmlParser::Token::Text && in_label_text) {
      label += p.text();
      continue;
    }
    if (t == XmlParser::Token::EndTag) {
      const std::string& tag = p.tag();
      if (tag == "text") {
        in_label_text = false;
      } else if (tag == "navpoint") {
        std::string title = trim(label);
        if (!title.empty() && !content.empty()) {
          TocEntry e;
          e.title = title;
          e.href = fs::normalize(fs::join_path(base, content));
          e.depth = std::max(0, depth);
          toc_.push_back(std::move(e));
        }
        depth = std::max(-1, depth - 1);
      }
    }
  }
}

void Book::load_stylesheets() {
  // Concatenating every stylesheet in the manifest is not strictly correct
  // (a chapter only links some of them) but books rarely conflict, and it
  // keeps chapter loading free of extra archive reads.
  for (const auto& kv : manifest_) {
    const std::string& href = kv.second;
    std::string type = manifest_types_.count(kv.first) ? manifest_types_[kv.first] : "";
    if (type != "text/css" && fs::extension(href) != "css") continue;
    std::string text;
    if (zip_.read(fs::normalize(fs::join_path(opf_dir_, href)), text)) css_.parse(text);
  }
}

bool Book::open_text() {
  if (!fs::read_file(path_, text_body_)) return false;
  if (text_body_.empty()) return false;
  // Split on blank lines near the target size so sections start at a
  // paragraph boundary.
  size_t pos = 0;
  while (pos < text_body_.size()) {
    size_t end = std::min(text_body_.size(), pos + kTextSectionBytes);
    if (end < text_body_.size()) {
      size_t split = text_body_.rfind("\n\n", end);
      if (split != std::string::npos && split > pos + kTextSectionBytes / 4) end = split + 2;
    }
    text_sections_.emplace_back(pos, end - pos);
    SpineItem item;
    item.id = ck::format("text%zu", text_sections_.size());
    item.href = ck::format("__text__/%zu", text_sections_.size() - 1);
    item.media_type = "text/plain";
    spine_.push_back(std::move(item));
    pos = end;
  }
  meta_.title = fs::stem(path_);
  format_ = Format::Text;
  CK_LOGI("text: \"%s\", %zu sections", meta_.title.c_str(), spine_.size());
  return true;
}

bool Book::open_cbz() {
  if (!zip_.open(path_)) return false;
  std::vector<std::string> images;
  for (const std::string& e : zip_.entries()) {
    std::string ext = fs::extension(e);
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "bmp") {
      images.push_back(e);
    }
  }
  if (images.empty()) return false;
  std::sort(images.begin(), images.end());
  for (const std::string& img : images) {
    SpineItem item;
    item.id = img;
    item.href = img;
    item.media_type = "image/*";
    spine_.push_back(std::move(item));
  }
  cover_href_ = images.front();
  meta_.title = fs::stem(path_);
  format_ = Format::Cbz;
  return true;
}

bool Book::read_chapter(int spine_index, std::string& out) const {
  if (spine_index < 0 || spine_index >= (int)spine_.size()) return false;
  if (format_ == Format::Text) {
    const auto& section = text_sections_[(size_t)spine_index];
    out = text_body_.substr(section.first, section.second);
    return true;
  }
  if (format_ == Format::Cbz) {
    out.clear();
    return true;  // the reader renders the image directly
  }
  return zip_.read(fs::normalize(fs::join_path(opf_dir_, spine_[spine_index].href)), out);
}

std::string Book::resolve_href(const std::string& href, const std::string& base_href) const {
  std::string clean = href;
  size_t hash = clean.find('#');
  if (hash != std::string::npos) clean = clean.substr(0, hash);
  if (clean.empty()) return "";
  if (starts_with(clean, "http://") || starts_with(clean, "https://") ||
      starts_with(clean, "mailto:")) {
    return "";
  }
  std::string base_dir = parent_dir(base_href);
  std::string joined = clean;
  if (!clean.empty() && clean[0] != '/') joined = fs::join_path(base_dir, clean);
  return fs::normalize(fs::join_path(opf_dir_, joined));
}

bool Book::read_resource(const std::string& href, const std::string& base_href,
                         std::string& out) const {
  if (format_ == Format::Cbz) return zip_.read(href, out);
  std::string path = resolve_href(href, base_href);
  if (path.empty()) return false;
  return zip_.read(path, out);
}

int Book::spine_index_for_href(const std::string& href) const {
  std::string clean = href;
  size_t hash = clean.find('#');
  if (hash != std::string::npos) clean = clean.substr(0, hash);
  clean = fs::normalize(clean);
  for (size_t i = 0; i < spine_.size(); ++i) {
    std::string candidate = fs::normalize(spine_[i].href);
    if (candidate == clean) return (int)i;
    // TOC hrefs are relative to the OPF directory, spine hrefs too, but
    // books disagree often enough that a basename match is worth trying.
    if (fs::basename(candidate) == fs::basename(clean)) return (int)i;
  }
  return -1;
}

std::string Book::fragment_of(const std::string& href) const {
  size_t hash = href.find('#');
  return hash == std::string::npos ? "" : href.substr(hash + 1);
}

bool Book::has_cover() const { return cover() != nullptr; }

const Canvas* Book::cover() const {
  if (cover_attempted_) return cover_cache_.get();
  cover_attempted_ = true;
  std::string href = cover_href_;
  if (href.empty()) {
    // Guess: an image whose name mentions "cover".
    for (const std::string& e : zip_.entries()) {
      std::string lower = to_lower(e);
      if (lower.find("cover") == std::string::npos) continue;
      std::string ext = fs::extension(e);
      if (ext == "jpg" || ext == "jpeg" || ext == "png") {
        href = e;
        break;
      }
    }
  }
  if (href.empty()) return nullptr;
  std::string data;
  if (!zip_.read(fs::normalize(fs::join_path(opf_dir_, href)), data) &&
      !zip_.read(href, data)) {
    return nullptr;
  }
  auto canvas = std::make_unique<Canvas>();
  if (!Canvas::decode_image(data.data(), data.size(), *canvas)) return nullptr;
  cover_cache_ = std::move(canvas);
  return cover_cache_.get();
}

uint64_t Book::spine_size(int index) const {
  if (index < 0 || index >= (int)spine_.size()) return 0;
  if (format_ == Format::Text) {
    return (uint64_t)text_sections_[(size_t)index].second;
  }
  return zip_.entry_size(fs::normalize(fs::join_path(opf_dir_, spine_[index].href)));
}

std::string Book::cache_key() const {
  // File size plus name: stable across a re-copy of the same book, and
  // distinct between two books that happen to share a title.
  uint64_t size = fs::file_size(path_);
  std::string base = fs::basename(path_);
  uint32_t hash = 2166136261u;
  for (char c : base) {
    hash ^= (uint8_t)c;
    hash *= 16777619u;
  }
  return ck::format("%08x_%llu", hash, (unsigned long long)size);
}

}  // namespace ck
