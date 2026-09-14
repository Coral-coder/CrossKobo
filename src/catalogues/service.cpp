#include "catalogues/service.h"

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "app/catalogue_file.h"
#include "app/settings.h"
#include "catalogues/page.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "core/version.h"
#include "net/http.h"
#include "net/libgen.h"
#include "net/opds.h"

namespace catalogues {
namespace {

using ck::Settings;

Response html(const std::string& body) {
  Response r;
  r.body = body;
  return r;
}

Response redirect(const std::string& to) {
  Response r;
  r.status = 302;
  r.location = to;
  r.body = "<a href=\"" + esc(to) + "\">" + esc(to) + "</a>";
  return r;
}

Response error_page(int status, const std::string& title, const std::string& message,
                    const std::string& back_href, const std::string& home_href = "/",
                    const std::string& home_label = "All catalogues",
                    const std::string& accent = "") {
  Response r;
  r.status = status;
  std::string body = notice("bad", "<p>" + esc(message) + "</p>");
  if (!back_href.empty()) body += button_link(back_href, "Back", true);
  body += button_link(home_href, home_label, true);
  r.body = page(title, body, back_href.empty() ? home_href : back_href, "Back", accent);
  return r;
}

std::vector<Settings::Catalogue> catalogues() {
  ensure_catalogue_file();
  return ck::load_catalogue_file();
}

// The request's own address, to come back to after a book page.
std::string self_url(const Request& request) {
  std::string out = request.path;
  std::string sep = "?";
  for (const auto& kv : request.query) {
    out += sep + ck::url_encode(kv.first) + "=" + ck::url_encode(kv.second);
    sep = "&";
  }
  return out;
}

std::string catalogue_path(size_t id) { return ck::format("/c/%zu", id); }

std::string kind_of(const Settings::Catalogue& c) {
  if (c.is_libgen()) return "search";
  if (c.search_only()) return "search";
  return "";
}

// True when some interface other than loopback has an address: the browser
// brought Wi-Fi up, or nothing has.
bool network_up() {
  struct ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return false;
  bool up = false;
  for (struct ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if (ifa->ifa_flags & IFF_LOOPBACK) continue;
    if (!(ifa->ifa_flags & IFF_UP)) continue;
    up = true;
    break;
  }
  freeifaddrs(list);
  return up;
}

// The menu entry asks the Kobo to connect before opening the browser, but
// nothing waits for that; the page arrives here while Wi-Fi is still
// coming up. So wait for it, within reason, before asking a server for
// anything - a request sent a second too early fails with a resolver error
// that tells the reader nothing.
bool wait_for_network(int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 500) {
    if (network_up()) return true;
    usleep(500 * 1000);
  }
  return false;
}

Response not_connected(const std::string& title, const std::string& retry_href,
                       const std::string& back_href, const std::string& accent = "") {
  Response r;
  r.status = 503;
  std::string body = notice("bad", "<p><b>Wi-Fi is not connected.</b></p>"
                                   "<p>Turn Wi-Fi on from the Kobo's menu (or close this "
                                   "window and open it again, which asks the Kobo to "
                                   "connect), then tap Try again.</p>");
  body += button_link(retry_href, "Try again");
  body += button_link(back_href, "Back", true);
  r.body = page(title, body, back_href, "Back", accent);
  return r;
}

std::string shown_path(const std::string& path) {
  const std::string& onboard = ck::paths().onboard;
  if (path.compare(0, onboard.size(), onboard) == 0 && path.size() > onboard.size()) {
    return path.substr(onboard.size() + 1);
  }
  return path;
}

// Ask Nickel to import newly downloaded books, so they show up in the
// library without the reader tapping Sync. Nickel imports whatever is new
// when a USB connection ends, so we replay that plug/unplug on the pipe it
// watches - the same mechanism the stock software and NickelMenu use. It
// runs in a detached child after a short pause, so the "Downloaded" page
// reaches the browser first. Best-effort: does nothing when the pipe is not
// there (not a Kobo, or Nickel not running).
const char* kNickelStatus = "/tmp/nickel-hardware-status";

bool library_import_available() { return ck::fs::exists(kNickelStatus); }

void trigger_library_import() {
  if (!library_import_available()) return;
  pid_t pid = fork();
  if (pid != 0) return;                 // the handler carries on and answers the browser
  setsid();                             // detach from this connection
  usleep(2500 * 1000);                  // let the Downloaded page render first
  int fd = open(kNickelStatus, O_WRONLY | O_NONBLOCK);
  if (fd >= 0) {
    auto put = [&](const char* s) { ssize_t n = write(fd, s, strlen(s)); (void)n; };
    put("usb plug add\n");
    usleep(1500 * 1000);
    put("usb plug remove\n");
    close(fd);
  }
  _exit(0);
}

// The line shown after a download, worded for whether the import will happen
// on its own or the reader still has to Sync.
std::string import_note() {
  if (library_import_available()) {
    return notice("note", "<p>Importing it into your library now - it will appear on your "
                          "home screen in a moment. The Kobo may flash its USB screen as it "
                          "imports; that is normal.</p>");
  }
  return notice("note", "<p>To see it in your library: close this window and tap <b>Sync</b>, "
                        "or <b>Catalogues - add downloads to library</b> in the menu.</p>");
}

// ------------------------------------------------------------------- pages

Response home(const Request& request) {
  std::vector<Settings::Catalogue> list = catalogues();

  // ?open=Name, from the menu entry that goes straight to one catalogue.
  std::string open = request.param("open");
  if (!open.empty()) {
    for (size_t i = 0; i < list.size(); ++i) {
      if (ck::iequals(list[i].name, open)) return redirect(catalogue_path(i));
    }
  }

  std::string body;
  if (!open.empty()) {
    body += notice("note", "<p>There is no catalogue called <b>" + esc(open) +
                               "</b> yet. Add a line for it to the catalogue file - see "
                               "<a href=\"/help\"><u>How this works</u></a>.</p>");
  }
  // The main screen is for catalogues you browse. Search services live in
  // Shelfmark, its own screen, and are not listed here.
  size_t shown = 0;
  for (size_t i = 0; i < list.size(); ++i) {
    const Settings::Catalogue& c = list[i];
    if (kind_of(c) == "search") continue;
    std::string meta = c.search_address();
    if (meta.size() > 60) meta = meta.substr(0, 58) + "…";
    body += row(catalogue_path(i), c.name, meta, kind_of(c));
    ++shown;
  }
  if (shown == 0) {
    body += notice("note", "<p>No catalogues to browse yet. Put one per line in <b>" +
                               esc(shown_path(ck::catalogue_file_path())) +
                               "</b> on the Kobo's drive. (Search servers go in "
                               "Shelfmark's own file and appear there, not here.)</p>");
  }
  body += "<h2>Downloads</h2>";
  body += row("/downloads", "Downloaded books", "What has been saved to the Downloads folder",
              "folder");
  body += row("/help", "How this works", "Adding your own servers and a friend's", "folder");
  std::string foot = "<div class=\"foot\">Catalogues " + std::string(ck::kVersion) +
                     (network_up() ? "" : " &middot; not connected to Wi-Fi") +
                     (ck::https_available() ? "" : " &middot; no https fetcher") +
                     "<br>Catalogue file: " + esc(shown_path(ck::catalogue_file_path())) +
                     "</div>";
  Response r = html(page("Catalogues", body) );
  r.body.insert(r.body.rfind("<script>"), foot);
  return r;
}

std::string entries_html(size_t id, const ck::OpdsFeed& feed, const std::string& back) {
  std::string body;
  for (const ck::OpdsEntry& e : feed.entries) {
    if (e.is_navigation()) {
      std::string meta = e.summary;
      if (meta.size() > 120) meta = meta.substr(0, 118) + "…";
      body += row(catalogue_path(id) + "?url=" + ck::url_encode(e.feed_url), e.title, meta,
                  "folder");
      continue;
    }
    if (e.download_url.empty()) continue;
    std::string href = catalogue_path(id) + "/book?url=" + ck::url_encode(e.download_url) +
                       "&title=" + ck::url_encode(e.title) + "&author=" +
                       ck::url_encode(e.author) + "&type=" + ck::url_encode(e.download_type) +
                       "&size=" + ck::url_encode(e.size > 0 ? ck::human_size((uint64_t)e.size) : "") +
                       "&back=" + ck::url_encode(back);
    std::string tag = ck::fs::extension(e.filename());
    body += row(href, e.title, e.author, ck::to_upper(tag), e.cover_url);
  }
  return body;
}

Response browse(const Request& request, size_t id, const Settings::Catalogue& c) {
  std::string here = self_url(request);
  if (c.search_only()) {
    std::string body = search_form(catalogue_path(id) + "/search", "", "Title or author");
    body += notice("note", "<p>" + esc(c.name) + " is searched rather than browsed. Type "
                           "something and tap Search.</p>");
    return html(page(c.name, body, "/", "Catalogues"));
  }
  std::string url = request.param("url");
  if (url.empty()) url = c.url;
  if (!wait_for_network(20000)) {
    return not_connected(c.name, here, url == c.url ? "/" : catalogue_path(id));
  }
  ck::OpdsFeed feed;
  std::string err;
  if (!ck::fetch_opds(url, c.user, c.password, feed, err)) {
    if (!network_up()) return not_connected(c.name, here, "/");
    return error_page(502, c.name, err, url == c.url ? "/" : catalogue_path(id));
  }
  bool searchable = !c.search.empty() || !feed.search_url.empty();
  std::string body;
  if (searchable) body += search_form(catalogue_path(id) + "/search", "", "Search " + c.name);
  if (feed.entries.empty()) body += notice("note", "<p>Nothing here.</p>");
  body += entries_html(id, feed, here);
  if (!feed.next_url.empty()) {
    body += button_link(catalogue_path(id) + "?url=" + ck::url_encode(feed.next_url),
                        "More", true);
  }
  std::string back = "/";
  std::string back_label = "Catalogues";
  if (url != c.url) {
    back = feed.up_url.empty() ? catalogue_path(id)
                               : catalogue_path(id) + "?url=" + ck::url_encode(feed.up_url);
    back_label = "Back";
  }
  std::string title = feed.title.empty() || url == c.url ? c.name : feed.title;
  return html(page(title, body, back, back_label));
}

std::string results_html(size_t id, const std::vector<ck::SearchResult>& results,
                         const std::string& back) {
  std::string body;
  for (const ck::SearchResult& r : results) {
    std::string href = catalogue_path(id) + "/book?md5=" + ck::url_encode(r.md5) +
                       "&url=" + ck::url_encode(r.direct_url) + "&title=" +
                       ck::url_encode(r.title) + "&author=" + ck::url_encode(r.author) +
                       "&ext=" + ck::url_encode(r.extension) + "&size=" +
                       ck::url_encode(r.size_text) + "&year=" + ck::url_encode(r.year) +
                       "&back=" + ck::url_encode(back);
    std::string meta = r.author;
    if (!r.year.empty()) meta += (meta.empty() ? "" : " · ") + r.year;
    if (!r.size_text.empty()) meta += (meta.empty() ? "" : " · ") + r.size_text;
    body += row(href, r.title, meta, ck::to_upper(r.extension));
  }
  return body;
}

Response search(const Request& request, size_t id, const Settings::Catalogue& c) {
  std::string q = ck::trim(request.param("q"));
  std::string here = self_url(request);
  std::string body = search_form(catalogue_path(id) + "/search", q, "Title or author");
  if (q.empty()) return html(page(c.name, body, catalogue_path(id)));
  if (!wait_for_network(20000)) return not_connected(c.name, here, catalogue_path(id));

  if (c.is_libgen()) {
    std::vector<ck::SearchResult> results;
    std::string err;
    if (!ck::libgen_search(c.search_address(), q, results, err)) {
      return error_page(502, c.name, err, catalogue_path(id));
    }
    if (results.empty()) body += notice("note", "<p>Nothing found for <b>" + esc(q) + "</b>.</p>");
    body += results_html(id, results, here);
    return html(page(c.name, body, catalogue_path(id)));
  }

  std::string tmpl = c.search;
  if (tmpl.empty()) {
    ck::OpdsFeed root;
    std::string err;
    if (!ck::fetch_opds(c.url, c.user, c.password, root, err)) {
      return error_page(502, c.name, err, catalogue_path(id));
    }
    tmpl = root.search_url;
  }
  if (tmpl.empty()) {
    return error_page(400, c.name, "This catalogue does not offer a search.", catalogue_path(id));
  }
  ck::OpdsFeed feed;
  std::string err;
  if (!ck::fetch_opds(ck::opds_search_url(tmpl, q), c.user, c.password, feed, err)) {
    return error_page(502, c.name, err, catalogue_path(id));
  }
  if (feed.entries.empty()) body += notice("note", "<p>Nothing found for <b>" + esc(q) + "</b>.</p>");
  body += entries_html(id, feed, here);
  if (!feed.next_url.empty()) {
    body += button_link(catalogue_path(id) + "?url=" + ck::url_encode(feed.next_url), "More",
                        true);
  }
  return html(page(c.name, body, catalogue_path(id)));
}

Response book(const Request& request, size_t id, const Settings::Catalogue& c) {
  std::string title = request.param("title");
  std::string author = request.param("author");
  std::string back = request.param("back");
  if (back.empty() || back[0] != '/') back = catalogue_path(id);
  if (title.empty()) title = "Untitled";

  std::string body = "<h2>" + esc(title) + "</h2>";
  if (!author.empty()) body += "<p>" + esc(author) + "</p>";
  std::string details;
  std::string ext = request.param("ext");
  std::string type = request.param("type");
  if (ext.empty() && !type.empty()) {
    // "application/epub+zip" is not what a reader wants to read.
    for (const char* known : {"kepub", "epub", "pdf", "mobi", "azw3", "cbz", "cbr", "txt"}) {
      if (ck::to_lower(type).find(known) != std::string::npos) {
        ext = known;
        break;
      }
    }
  }
  details += ext.empty() ? type : ck::to_upper(ext);
  if (!request.param("year").empty()) details += (details.empty() ? "" : " · ") + request.param("year");
  if (!request.param("size").empty()) details += (details.empty() ? "" : " · ") + request.param("size");
  if (!details.empty()) body += "<p class=\"small\">" + esc(details) + "</p>";
  body += "<p class=\"small\">Saves to the Downloads folder on this Kobo.</p>";

  std::vector<std::pair<std::string, std::string>> fields = {
      {"title", title}, {"author", author}, {"url", request.param("url")},
      {"type", type},   {"md5", request.param("md5")}, {"ext", ext},
      {"back", back},
  };
  body += button_form(catalogue_path(id) + "/download", fields, "Download", "Downloading…");
  body += button_link(back, "Back", true);
  return html(page(c.name, body, back));
}

Response download(const Request& request, size_t id, const Settings::Catalogue& c) {
  if (request.method != "POST") return redirect(catalogue_path(id));
  std::string back = request.param("back");
  if (back.empty() || back[0] != '/') back = catalogue_path(id);
  if (!wait_for_network(20000)) return not_connected("Not downloaded", back, back);
  std::string path;
  std::string err;
  bool ok;
  if (c.is_libgen()) {
    ck::SearchResult r;
    r.title = request.param("title");
    r.author = request.param("author");
    r.md5 = request.param("md5");
    r.extension = request.param("ext");
    r.direct_url = request.param("url");
    ok = ck::libgen_download(c.search_address(), r, download_dir(), c.user, c.password, path,
                             err);
  } else {
    ck::OpdsEntry e;
    e.title = request.param("title");
    e.author = request.param("author");
    e.download_url = request.param("url");
    e.download_type = request.param("type");
    ok = ck::download_opds_entry(e, download_dir(), c.user, c.password, path, err);
  }
  if (!ok) {
    CK_LOGW("download failed: %s", err.c_str());
    return error_page(502, "Not downloaded", err, back);
  }
  CK_LOGI("downloaded %s", path.c_str());
  trigger_library_import();
  std::string body = notice("good", "<p><b>" + esc(request.param("title")) +
                                        "</b> is on this Kobo, as<br>" + esc(shown_path(path)) +
                                        "</p>");
  body += import_note();
  body += button_link(back, "More books", true);
  body += button_link("/downloads", "Downloaded books", true);
  return html(page("Downloaded", body, back));
}

Response downloads() {
  std::vector<ck::fs::Entry> entries = ck::fs::list_dir(download_dir());
  std::sort(entries.begin(), entries.end(),
            [](const ck::fs::Entry& a, const ck::fs::Entry& b) { return a.mtime > b.mtime; });
  std::string body;
  if (entries.empty()) body += notice("note", "<p>Nothing downloaded yet.</p>");
  for (const ck::fs::Entry& e : entries) {
    if (e.is_dir) continue;
    body += "<div class=\"row\"><span class=\"g\">" + esc(ck::human_size(e.size)) +
            "</span><span class=\"t\">" + esc(e.name) + "</span></div>";
  }
  body += "<p class=\"small\">Folder: " + esc(shown_path(download_dir())) +
          ". Tap <b>Sync</b> on the home screen, or <b>Catalogues - add downloads to "
          "library</b> in the menu, and the Kobo adds these to your library.</p>";
  return html(page("Downloaded books", body, "/", "Catalogues"));
}

Response help() {
  std::string file = shown_path(ck::catalogue_file_path());
  std::string body =
      "<p>Every catalogue on the first page comes from one file on this Kobo's drive:</p>"
      "<pre>" + esc(file) + "</pre>"
      "<p>Plug the Kobo into a computer and open it in any text editor. One server per "
      "line:</p>"
      "<pre>Name | address\nName | address | user | password\nName | address | libgen</pre>"
      "<p>Use a hostname rather than an IP number, and the same line works at home and "
      "away. The kind of server is worked out from the address: <b>search.php</b> or "
      "<b>json.php</b> means a search-format server, <b>{searchTerms}</b> a search "
      "endpoint, anything else an OPDS feed. Say <b>libgen</b> or <b>opds</b> on the line "
      "if the guess is wrong.</p>"
      "<h2>Shelfmark, Calibre-Web, Kavita, Komga</h2>"
      "<p>Any of these speaks OPDS. Find the OPDS address in its settings (for Shelfmark it "
      "is <b>/opds</b> on the server) and add a line with it, plus a user and password if "
      "it asks for one:</p>"
      "<pre>Calibre-Web | https://books.example.net/opds | me | secret</pre>"
      "<p><b>Shelfmark</b> is separate: it is a search engine with its own menu entry "
      "and its own file, <b>shelfmark.txt</b>. Nothing here touches it.</p>"
      "<h2>A search-format server</h2>"
      "<p>A server that answers <b>search.php?req=</b> or <b>json.php</b> the way Library "
      "Genesis forks do is searched in that format. Give it the search address:</p>"
      "<pre>Fic | https://fic.example.net/search.php?req={searchTerms}</pre>"
      "<p>No addresses of that kind are shipped; the file lists only what you put in it, "
      "and the two public catalogues it starts with.</p>"
      "<h2>Downloads</h2>"
      "<p>Books go into the <b>Downloads</b> folder on the drive. The Kobo adds them to "
      "your library when it next looks: tap <b>Sync</b> on the home screen, or "
      "<b>Catalogues - add downloads to library</b> in the menu.</p>"
      "<h2>Your own certificate</h2>"
      "<p>A server with a certificate of its own can be trusted by putting the "
      "certificate, in PEM form, at <b>" + esc(shown_path(ck::paths().data + "/extra-ca.pem")) +
      "</b>.</p>"
      "<h2>Removing this</h2>"
      "<p>Unzip the uninstaller onto the drive and eject; a <b>Remove Catalogues</b> entry "
      "then appears in the menu, and one tap takes it all away.</p>";
  return html(page("How this works", body, "/", "Catalogues"));
}

Response status() {
  Response r;
  r.content_type = "application/json; charset=utf-8";
  r.body = "{\"ok\":true,\"version\":\"" + json_escape(ck::kVersion) + "\",\"wifi\":" +
           (network_up() ? "true" : "false") + ",\"https\":" +
           (ck::https_available() ? "true" : "false") + ",\"downloads\":\"" +
           json_escape(download_dir()) + "\",\"file\":\"" +
           json_escape(ck::catalogue_file_path()) + "\"}\n";
  return r;
}


// ============================= Shelfmark ==================================
//
// Shelfmark is a search engine, not a feed browser. It has its own sources
// file - shelfmark.txt, never catalogues.txt - and its own look. You open
// it, you type, it searches every server listed there at once, and every
// hit is one tap to download. The servers are search-format ones (the
// libgen search shape a lot of self-hosted things speak), and OPDS servers
// that advertise a search are searched too. No addresses are shipped.

const char* kShelfmarkAccent = "#0369a1";   // calibrain's sky-blue primary

std::string shelfmark_file_path() { return ck::paths().data + "/shelfmark.txt"; }

std::vector<Settings::Catalogue> shelfmark_sources() {
  std::string text;
  if (!ck::fs::read_file(shelfmark_file_path(), text)) return {};
  std::vector<Settings::Catalogue> src = ck::parse_catalogue_list(text);
  // Shelfmark is a search engine in the Library-Genesis shape - a web search
  // that answers with an HTML table or JSON, not an OPDS feed. That is the
  // whole point of it, so every source here is treated as one, whatever the
  // address looks like: the reader never has to tag their own server, and a
  // plain base URL (the code probes search.php / json.php under it) works as
  // well as a full search URL.
  for (Settings::Catalogue& c : src) {
    c.format = "libgen";
    if (c.url.empty() && !c.search.empty()) {
      c.url = c.search;
      c.search.clear();
    }
  }
  return src;
}

Response sm_page(const std::string& title, const std::string& body,
                 const std::string& back_href = "", const std::string& back_label = "Back") {
  return html(page(title, body, back_href, back_label, kShelfmarkAccent));
}

// A calibrain-style result card: cover on top, title, author, a dotted meta
// line, and a full-width Download bar flush at the bottom. The whole card is
// the link to the book's page, where the download happens.
std::string sm_card(const std::string& href, const std::string& title,
                    const std::string& author, const std::string& meta,
                    const std::string& cover) {
  std::string out = "<a class=\"card\" href=\"" + esc(href) + "\">";
  out += "<div class=\"cover\">";
  if (!cover.empty()) out += "<img src=\"" + esc(cover) + "\" alt=\"\">";
  else out += "No Cover";
  out += "</div><div class=\"cb\"><div class=\"ct\">" +
         esc(title.empty() ? "Untitled" : title) + "</div>";
  if (!author.empty()) out += "<div class=\"ca\">" + esc(author) + "</div>";
  if (!meta.empty()) out += "<div class=\"cm\">" + esc(meta) + "</div>";
  out += "</div><div class=\"cd\">Download</div></a>\n";
  return out;
}

// One result, whichever kind of server it came from. `s` is the source
// index, so the download knows which server (and which credentials) to use.
std::string sm_libgen_row(size_t s, const std::string& label, const ck::SearchResult& r,
                          const std::string& back) {
  std::string href = "/shelfmark/book?s=" + ck::format("%zu", s) + "&md5=" + ck::url_encode(r.md5) +
                     "&url=" + ck::url_encode(r.direct_url) + "&title=" + ck::url_encode(r.title) +
                     "&author=" + ck::url_encode(r.author) + "&ext=" + ck::url_encode(r.extension) +
                     "&size=" + ck::url_encode(r.size_text) + "&year=" + ck::url_encode(r.year) +
                     "&back=" + ck::url_encode(back);
  // year · FORMAT · size · [server], the way calibrain's card meta reads.
  std::string meta;
  auto add = [&](const std::string& s) {
    if (!s.empty()) meta += (meta.empty() ? "" : " · ") + s;
  };
  add(r.year);
  add(ck::to_upper(r.extension));
  add(r.size_text);
  add(label);
  return sm_card(href, r.title, r.author, meta, r.cover_url);
}

std::string sm_opds_row(size_t s, const std::string& label, const ck::OpdsEntry& e,
                        const std::string& back) {
  std::string href = "/shelfmark/book?s=" + ck::format("%zu", s) + "&url=" +
                     ck::url_encode(e.download_url) + "&title=" + ck::url_encode(e.title) +
                     "&author=" + ck::url_encode(e.author) + "&type=" +
                     ck::url_encode(e.download_type) + "&back=" + ck::url_encode(back);
  std::string meta = ck::to_upper(ck::fs::extension(e.filename()));
  if (!label.empty()) meta += (meta.empty() ? "" : " · ") + label;
  return sm_card(href, e.title, e.author, meta, e.cover_url);
}

std::string shelfmark_setup_note() {
  return notice("note",
                "<p><b>No search servers yet.</b></p>"
                "<p>Shelfmark searches the servers you list in this file on the Kobo's "
                "drive:</p><pre>" + esc(shown_path(shelfmark_file_path())) + "</pre>"
                "<p>Plug the Kobo into a computer and add one per line. It is a "
                "Library-Genesis-style search server - give its address, either the base "
                "URL or a full search URL:</p>"
                "<pre>My server | https://fic.example.net\n"
                "My server | https://fic.example.net/search.php?req={searchTerms}</pre>"
                "<p>Every line here is searched as that kind of server; no OPDS, no tags "
                "needed. It is Shelfmark's own file - your OPDS catalogues stay in "
                "catalogues.txt and are browsed from Catalogues.</p>");
}

Response shelfmark_home(const Request& request) {
  std::vector<Settings::Catalogue> src = shelfmark_sources();
  std::string body = search_form("/shelfmark/search", "", "Search for a title or author");
  if (src.empty()) {
    body += shelfmark_setup_note();
  } else {
    std::string names;
    for (size_t i = 0; i < src.size(); ++i) names += (i ? ", " : "") + src[i].name;
    body += notice("note", "<p>Type above to search: <b>" + esc(names) + "</b>.</p>");
  }
  body += "<div class=\"foot\">Shelfmark " + std::string(ck::kVersion) +
          (network_up() ? "" : " &middot; not connected to Wi-Fi") +
          (ck::https_available() ? "" : " &middot; no https fetcher") +
          "<br>Search servers: " + esc(shown_path(shelfmark_file_path())) + "</div>";
  return sm_page("Shelfmark", body);
}

Response shelfmark_search(const Request& request) {
  std::string q = ck::trim(request.param("q"));
  std::vector<Settings::Catalogue> src = shelfmark_sources();
  std::string here = self_url(request);
  std::string body = search_form("/shelfmark/search", q, "Search for a title or author");
  if (src.empty()) {
    body += shelfmark_setup_note();
    return sm_page("Shelfmark", body);
  }
  if (q.empty()) return sm_page("Shelfmark", body);
  if (!wait_for_network(20000)) {
    return not_connected("Shelfmark", here, "/shelfmark", kShelfmarkAccent);
  }

  int found = 0;
  std::string rows;
  std::string trouble;
  bool multi = src.size() > 1;
  for (size_t i = 0; i < src.size(); ++i) {
    const Settings::Catalogue& c = src[i];
    std::string label = multi ? c.name : "";
    if (c.is_libgen()) {
      std::vector<ck::SearchResult> results;
      std::string err;
      if (!ck::libgen_search(c.search_address(), q, results, err)) {
        trouble += "<p>" + esc(c.name) + ": " + esc(err) + "</p>";
        continue;
      }
      for (const ck::SearchResult& r : results) {
        rows += sm_libgen_row(i, label, r, here);
        ++found;
      }
    } else {
      std::string tmpl = c.search;
      std::string err;
      if (tmpl.empty()) {
        ck::OpdsFeed root;
        if (ck::fetch_opds(c.url, c.user, c.password, root, err)) tmpl = root.search_url;
      }
      if (tmpl.empty()) {
        trouble += "<p>" + esc(c.name) + ": offers no search.</p>";
        continue;
      }
      ck::OpdsFeed feed;
      if (!ck::fetch_opds(ck::opds_search_url(tmpl, q), c.user, c.password, feed, err)) {
        trouble += "<p>" + esc(c.name) + ": " + esc(err) + "</p>";
        continue;
      }
      for (const ck::OpdsEntry& e : feed.entries) {
        if (e.download_url.empty()) continue;
        rows += sm_opds_row(i, label, e, here);
        ++found;
      }
    }
  }
  if (found == 0) {
    body += notice("note", "<p>Nothing found for <b>" + esc(q) + "</b>.</p>");
    // The parser saw an answer it could not read: it saved exactly what the
    // server sent. Say where, so the markup can be looked at.
    std::string dump = ck::paths().data + "/last-search.html";
    if (ck::fs::exists(dump)) {
      body += notice("note", "<p>The server answered but nothing could be read from it. "
                             "The raw answer was saved to <b>" + esc(shown_path(dump)) +
                             "</b> on the Kobo's drive.</p>");
    }
  }
  if (!rows.empty()) body += "<div class=\"grid\">" + rows + "</div>";
  if (!trouble.empty()) body += notice("bad", trouble);
  return sm_page("Shelfmark", body, "/shelfmark", "Shelfmark");
}

Response shelfmark_book(const Request& request) {
  std::vector<Settings::Catalogue> src = shelfmark_sources();
  int s = ck::to_int(request.param("s"), -1);
  if (s < 0 || s >= (int)src.size()) {
    return error_page(404, "Shelfmark", "That result is gone - search again.", "/shelfmark",
                      "/shelfmark", "Shelfmark", kShelfmarkAccent);
  }
  std::string title = request.param("title");
  std::string author = request.param("author");
  std::string back = request.param("back");
  if (back.empty() || back[0] != '/') back = "/shelfmark";
  if (title.empty()) title = "Untitled";

  std::string body = "<h2>" + esc(title) + "</h2>";
  if (!author.empty()) body += "<p>" + esc(author) + "</p>";
  std::string ext = request.param("ext");
  std::string details = ext.empty() ? request.param("type") : ck::to_upper(ext);
  if (!request.param("year").empty()) details += (details.empty() ? "" : " · ") + request.param("year");
  if (!request.param("size").empty()) details += (details.empty() ? "" : " · ") + request.param("size");
  if (src.size() > 1) details += (details.empty() ? "" : " · ") + src[(size_t)s].name;
  if (!details.empty()) body += "<p class=\"small\">" + esc(details) + "</p>";
  body += "<p class=\"small\">Saves to the Downloads folder on this Kobo.</p>";

  std::vector<std::pair<std::string, std::string>> fields = {
      {"s", request.param("s")}, {"title", title}, {"author", author},
      {"url", request.param("url")}, {"type", request.param("type")},
      {"md5", request.param("md5")}, {"ext", ext}, {"back", back},
  };
  body += button_form("/shelfmark/download", fields, "Download", "Downloading…");
  body += button_link(back, "Back to results", true);
  return sm_page("Shelfmark", body, back, "Back");
}

Response shelfmark_download(const Request& request) {
  if (request.method != "POST") return redirect("/shelfmark");
  std::vector<Settings::Catalogue> src = shelfmark_sources();
  int s = ck::to_int(request.param("s"), -1);
  if (s < 0 || s >= (int)src.size()) {
    return error_page(404, "Shelfmark", "That result is gone - search again.", "/shelfmark",
                      "/shelfmark", "Shelfmark", kShelfmarkAccent);
  }
  const Settings::Catalogue& c = src[(size_t)s];
  std::string back = request.param("back");
  if (back.empty() || back[0] != '/') back = "/shelfmark";
  if (!wait_for_network(20000)) {
    return not_connected("Shelfmark", back, back, kShelfmarkAccent);
  }
  std::string path;
  std::string err;
  bool ok;
  if (c.is_libgen()) {
    ck::SearchResult r;
    r.title = request.param("title");
    r.author = request.param("author");
    r.md5 = request.param("md5");
    r.extension = request.param("ext");
    r.direct_url = request.param("url");
    ok = ck::libgen_download(c.search_address(), r, download_dir(), c.user, c.password, path, err);
  } else {
    ck::OpdsEntry e;
    e.title = request.param("title");
    e.author = request.param("author");
    e.download_url = request.param("url");
    e.download_type = request.param("type");
    ok = ck::download_opds_entry(e, download_dir(), c.user, c.password, path, err);
  }
  if (!ok) {
    CK_LOGW("shelfmark download failed: %s", err.c_str());
    return error_page(502, "Not downloaded", err, back, "/shelfmark", "Shelfmark",
                      kShelfmarkAccent);
  }
  CK_LOGI("shelfmark downloaded %s", path.c_str());
  trigger_library_import();
  std::string body = notice("good", "<p><b>" + esc(request.param("title")) +
                                        "</b> is on this Kobo, as<br>" + esc(shown_path(path)) +
                                        "</p>");
  body += import_note();
  body += button_link(back, "More results", true);
  return sm_page("Downloaded", body, back, "Back");
}

}  // namespace

std::string download_dir() { return ck::paths().onboard + "/Downloads"; }

std::string catalogue_file_template() {
  return "# Catalogues\n"
         "#\n"
         "# One server per line. Edit this over USB, or paste in a line a friend\n"
         "# sent you; the page reads it fresh every time it opens.\n"
         "#\n"
         "#   Name | address\n"
         "#   Name | address | user | password\n"
         "#   Name | address | libgen\n"
         "#\n"
         "# Use a hostname rather than an IP number, so the same line works at\n"
         "# home and away. The kind of server is worked out from the address:\n"
         "# search.php or json.php means a search-format server, {searchTerms} a\n"
         "# search endpoint, anything else an OPDS feed. Say libgen or opds\n"
         "# outright if the guess is wrong.\n"
         "#\n"
         "# Examples of your own, commented out:\n"
         "# Shelfmark   | https://books.example.net/opds | me | secret\n"
         "# My server   | https://fic.example.net/search.php?req={searchTerms}\n"
         "\n"
         "Project Gutenberg | https://m.gutenberg.org/ebooks.opds/\n"
         "Internet Archive  | https://bookserver.archive.org/catalog/\n";
}

void ensure_catalogue_file() {
  std::string path = ck::paths().data + "/catalogues.txt";
  if (ck::fs::exists(path) || ck::fs::exists(ck::paths().data + "/catalogues.json")) return;
  ck::fs::mkdir_p(ck::paths().data);
  if (ck::fs::write_file_atomic(path, catalogue_file_template())) {
    CK_LOGI("wrote %s", path.c_str());
  }
}

void ensure_shelfmark_file() {
  std::string path = ck::paths().data + "/shelfmark.txt";
  if (ck::fs::exists(path)) return;
  ck::fs::mkdir_p(ck::paths().data);
  const char* kTemplate =
      "# Shelfmark - search servers\n"
      "#\n"
      "# Shelfmark is a search engine. It has nothing to do with catalogues.txt:\n"
      "# this file, and only this file, is the list of servers Shelfmark searches.\n"
      "#\n"
      "# One server per line. Use a hostname, not an IP number, so the same line\n"
      "# works at home and away:\n"
      "#\n"
      "#   Name | https://host\n"
      "#   Name | https://host/search.php?req={searchTerms}\n"
      "#   Name | https://host | user | password\n"
      "#\n"
      "# Every line is searched as a Library-Genesis-style server (an HTML or\n"
      "# JSON search, not OPDS) - that is what Shelfmark is, so no tag is\n"
      "# needed. Give the base URL and it probes search.php / json.php under\n"
      "# it, or give a full search URL. No servers are shipped - add yours.\n"
      "\n";
  if (ck::fs::write_file_atomic(path, kTemplate)) {
    CK_LOGI("wrote %s", path.c_str());
  }
}

Response handle(const Request& request) {
  const std::string& path = request.path;
  if (path == "/" || path == "/index.html") return home(request);
  if (path == "/downloads") return downloads();
  if (path == "/help") return help();
  if (path == "/status") return status();
  if (path == "/shelfmark") return shelfmark_home(request);
  if (path == "/shelfmark/search") return shelfmark_search(request);
  if (path == "/shelfmark/book") return shelfmark_book(request);
  if (path == "/shelfmark/download") return shelfmark_download(request);

  if (ck::starts_with(path, "/c/")) {
    std::string rest = path.substr(3);
    size_t slash = rest.find('/');
    std::string number = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string action = slash == std::string::npos ? "" : rest.substr(slash + 1);
    std::vector<Settings::Catalogue> list = catalogues();
    int id = ck::to_int(number, -1);
    if (number.empty() || id < 0 || id >= (int)list.size()) {
      return error_page(404, "Catalogues", "That catalogue is not in the list any more.", "/");
    }
    const Settings::Catalogue& c = list[(size_t)id];
    if (action.empty()) return browse(request, (size_t)id, c);
    if (action == "search") return search(request, (size_t)id, c);
    if (action == "book") return book(request, (size_t)id, c);
    if (action == "download") return download(request, (size_t)id, c);
  }
  return error_page(404, "Catalogues", "There is no such page.", "/");
}

}  // namespace catalogues
