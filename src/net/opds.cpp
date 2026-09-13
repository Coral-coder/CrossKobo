#include "net/opds.h"

#include <algorithm>
#include <cstring>

#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "epub/xml.h"
#include "net/http.h"

namespace ck {
namespace {

// Link relations worth acting on. Everything else in a feed is decoration.
bool is_acquisition(const std::string& rel) {
  if (rel.empty()) return false;
  return rel.find("acquisition") != std::string::npos;
}

bool is_image(const std::string& rel) {
  return rel.find("image") != std::string::npos || rel.find("thumbnail") != std::string::npos;
}

bool is_feed_type(const std::string& type) {
  return type.find("atom+xml") != std::string::npos || type.find("opds") != std::string::npos;
}

std::string extension_for(const std::string& type) {
  if (type.find("epub") != std::string::npos) return ".epub";
  if (type.find("pdf") != std::string::npos) return ".pdf";
  if (type.find("cbz") != std::string::npos || type.find("comicbook") != std::string::npos) {
    return ".cbz";
  }
  if (type.find("plain") != std::string::npos) return ".txt";
  if (type.find("mobi") != std::string::npos) return ".mobi";
  return "";
}

}  // namespace

std::string OpdsEntry::filename() const {
  std::string base = title.empty() ? "book" : title;
  if (!author.empty()) base = author + " - " + base;
  base = fs::sanitize_filename(base);
  std::string ext = extension_for(download_type);
  if (ext.empty()) {
    // Fall back to whatever the URL ends with, when it looks like a file.
    size_t slash = download_url.find_last_of('/');
    std::string tail = slash == std::string::npos ? download_url : download_url.substr(slash + 1);
    size_t dot = tail.find_last_of('.');
    if (dot != std::string::npos && tail.size() - dot <= 6) ext = tail.substr(dot);
  }
  if (ext.empty()) ext = ".epub";
  return base + ext;
}

std::string resolve_url(const std::string& base, const std::string& href) {
  if (href.empty()) return "";
  if (href.find("://") != std::string::npos) return href;
  Url b;
  if (!Url::parse(base, b)) return href;
  std::string origin = b.scheme + "://" + b.host;
  if (b.port > 0 && !((b.scheme == "http" && b.port == 80) ||
                      (b.scheme == "https" && b.port == 443))) {
    origin += format(":%d", b.port);
  }
  if (href[0] == '/') return origin + href;
  // Relative to the directory of the base path.
  std::string dir = b.path;
  size_t slash = dir.find_last_of('/');
  dir = slash == std::string::npos ? "/" : dir.substr(0, slash + 1);
  return origin + dir + href;
}

std::string opds_search_url(const std::string& tmpl, const std::string& terms) {
  if (tmpl.empty()) return "";
  std::string encoded = url_encode(terms);
  std::string out = tmpl;
  const char* kTokens[] = {"{searchTerms}", "{searchterms}", "{query}"};
  bool replaced = false;
  for (const char* token : kTokens) {
    size_t at = out.find(token);
    while (at != std::string::npos) {
      out.replace(at, strlen(token), encoded);
      replaced = true;
      at = out.find(token);
    }
  }
  if (!replaced) {
    // Some catalogues advertise a bare endpoint and expect ?q=
    out += (out.find('?') == std::string::npos ? "?q=" : "&q=") + encoded;
  }
  return out;
}

bool parse_opds(const std::string& xml, const std::string& base_url, OpdsFeed& out) {
  out = OpdsFeed();
  XmlParser parser(xml);
  bool in_entry = false;
  bool saw_feed = false;
  OpdsEntry entry;
  // Which element's character data we are collecting, and for whom.
  std::string collecting;
  std::string text;
  int author_depth = 0;

  auto finish_text = [&]() {
    std::string value = trim(text);
    text.clear();
    if (value.empty()) return;
    if (collecting == "title") {
      if (in_entry) {
        if (entry.title.empty()) entry.title = value;
      } else if (out.title.empty()) {
        out.title = value;
      }
    } else if (collecting == "name" && author_depth > 0 && in_entry) {
      if (entry.author.empty()) entry.author = value;
    } else if ((collecting == "summary" || collecting == "content") && in_entry) {
      if (entry.summary.empty()) entry.summary = xml_text_content(value);
    } else if (collecting == "updated" && in_entry) {
      if (entry.updated.empty()) entry.updated = value;
    }
    collecting.clear();
  };

  while (true) {
    XmlParser::Token token = parser.next();
    if (token == XmlParser::Token::End) break;
    if (token == XmlParser::Token::Text) {
      if (!collecting.empty()) text += parser.text();
      continue;
    }
    if (token != XmlParser::Token::StartTag && token != XmlParser::Token::EndTag) continue;

    std::string tag = parser.tag();
    // Namespace prefixes vary between servers; only the local name matters.
    size_t colon = tag.find(':');
    if (colon != std::string::npos) tag = tag.substr(colon + 1);

    if (token == XmlParser::Token::StartTag) {
      if (tag == "feed") {
        saw_feed = true;
      } else if (tag == "entry") {
        in_entry = true;
        entry = OpdsEntry();
      } else if (tag == "author") {
        ++author_depth;
      } else if (tag == "title" || tag == "name" || tag == "summary" || tag == "content" ||
                 tag == "updated") {
        finish_text();
        collecting = tag;
        text.clear();
      } else if (tag == "link") {
        std::string rel = to_lower(parser.attr("rel"));
        std::string type = to_lower(parser.attr("type"));
        std::string href = resolve_url(base_url, parser.attr("href"));
        if (href.empty()) continue;
        if (in_entry) {
          if (is_acquisition(rel)) {
            // Prefer a format the reader can actually open.
            std::string ext = extension_for(type);
            bool better = entry.download_url.empty() ||
                          (extension_for(entry.download_type).empty() && !ext.empty()) ||
                          (ext == ".epub" && extension_for(entry.download_type) != ".epub");
            if (better) {
              entry.download_url = href;
              entry.download_type = type;
              entry.size = (int64_t)to_int(parser.attr("length"), 0);
            }
          } else if (is_image(rel)) {
            if (entry.cover_url.empty() || rel.find("thumbnail") != std::string::npos) {
              entry.cover_url = href;
            }
          } else if (is_feed_type(type) || rel == "subsection") {
            if (entry.feed_url.empty()) entry.feed_url = href;
          }
        } else {
          if (rel == "next") {
            out.next_url = href;
          } else if (rel == "up") {
            out.up_url = href;
          } else if (rel == "search") {
            // Either a template on the link itself or an OpenSearch
            // description to fetch later; both come back here as a URL.
            out.search_url = href;
          }
        }
      }
      if (parser.self_closing() && tag == "entry") in_entry = false;
      continue;
    }

    // End tag.
    if (!collecting.empty() && tag == collecting) finish_text();
    if (tag == "author") {
      if (author_depth > 0) --author_depth;
    } else if (tag == "entry") {
      finish_text();
      if (!entry.title.empty() || !entry.download_url.empty() || !entry.feed_url.empty()) {
        out.entries.push_back(entry);
      }
      in_entry = false;
    }
  }
  return saw_feed || !out.entries.empty();
}

bool fetch_opds(const std::string& url, const std::string& user, const std::string& password,
                OpdsFeed& out, std::string& error) {
  HttpRequest request;
  if (!Url::parse(url, request.url)) {
    error = "That does not look like a web address.";
    return false;
  }
  request.headers["Accept"] = "application/atom+xml, application/xml, */*";
  if (!user.empty()) {
    request.headers["Authorization"] = "Basic " + base64_encode(user + ":" + password);
  }
  HttpResponse response = http_perform(request);
  if (!response.error.empty()) {
    error = response.error;
    return false;
  }
  if (response.status == 401) {
    error = "The catalogue wants a username and password.";
    return false;
  }
  if (!response.ok()) {
    error = format("The catalogue answered %d.", response.status);
    return false;
  }
  if (!parse_opds(response.body, url, out)) {
    error = "That address did not return an OPDS catalogue.";
    return false;
  }
  CK_LOGI("opds: %s -> \"%s\", %zu entries", url.c_str(), out.title.c_str(), out.entries.size());
  return true;
}

bool download_opds_entry(const OpdsEntry& entry, const std::string& dir,
                         const std::string& user, const std::string& password,
                         std::string& path_out, std::string& error) {
  if (entry.download_url.empty()) {
    error = "That entry has nothing to download.";
    return false;
  }
  fs::mkdir_p(dir);
  std::string target = dir + "/" + entry.filename();
  // Never overwrite: a catalogue is not authoritative over the drive.
  if (fs::exists(target)) {
    std::string base = target;
    std::string ext;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) {
      ext = base.substr(dot);
      base = base.substr(0, dot);
    }
    for (int i = 2; i < 100 && fs::exists(target); ++i) {
      target = format("%s (%d)%s", base.c_str(), i, ext.c_str());
    }
  }
  HttpRequest request;
  if (!Url::parse(entry.download_url, request.url)) {
    error = "That download address is not valid.";
    return false;
  }
  request.download_path = target;
  request.timeout_ms = 120000;
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
    error = "The catalogue sent an empty file.";
    return false;
  }
  path_out = target;
  CK_LOGI("opds: downloaded %s (%llu bytes)", target.c_str(),
          (unsigned long long)fs::file_size(target));
  return true;
}

}  // namespace ck
