// The catalogues program, end to end: a fake OPDS server and a fake
// search-format server on loopback, a catalogue file pointing at both, and
// every page the Kobo's browser would ask for - through the real HTTP
// server, over a real socket, the way the browser does it.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <thread>

#include "app/catalogue_file.h"
#include "catalogues/httpd.h"
#include "catalogues/service.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "net/http.h"

using namespace ck;
using catalogues::Request;
using catalogues::Response;

static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

namespace {

const int kFakePort = 46521;
const int kAppPort = 46522;
const char* kEpubBytes = "PK\x03\x04 not really an epub, but bytes are bytes";

std::string fake_base() { return format("http://127.0.0.1:%d", kFakePort); }

// A library with one folder, one book, a search, and a search-format
// endpoint - the same kinds of answer a real Shelfmark or a real fork of
// the libgen software give.
Response fake_server(const Request& request) {
  Response r;
  const std::string& p = request.path;
  if (p == "/opds") {
    r.content_type = "application/atom+xml";
    r.body =
        "<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\">"
        "<title>Fake Library</title>"
        "<link rel=\"search\" type=\"application/atom+xml\" href=\"/opds/search?q={searchTerms}\"/>"
        "<entry><title>Fiction</title><link rel=\"subsection\" "
        "type=\"application/atom+xml;profile=opds-catalog;kind=acquisition\" href=\"/opds/fiction\"/>"
        "<content>Made-up things</content></entry>"
        "<entry><title>Moby Dick</title><author><name>Herman Melville</name></author>"
        "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" "
        "href=\"/files/moby.epub\" length=\"44\"/>"
        "<link rel=\"http://opds-spec.org/image/thumbnail\" href=\"/covers/moby.jpg\"/>"
        "</entry></feed>";
    return r;
  }
  if (p == "/opds/fiction") {
    r.content_type = "application/atom+xml";
    r.body =
        "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Fiction</title>"
        "<link rel=\"up\" href=\"/opds\"/>"
        "<link rel=\"next\" href=\"/opds/fiction?page=2\"/>"
        "<entry><title>Tom &amp; Jerry</title><author><name>Anon</name></author>"
        "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" "
        "href=\"/files/tom.epub\"/></entry></feed>";
    return r;
  }
  if (p == "/opds/search") {
    r.content_type = "application/atom+xml";
    r.body = "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Results for " +
             catalogues::html_escape(request.param("q")) + "</title>"
             "<entry><title>Found: " + catalogues::html_escape(request.param("q")) + "</title>"
             "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" "
             "href=\"/files/found.epub\"/></entry></feed>";
    return r;
  }
  if (starts_with(p, "/files/")) {
    r.content_type = "application/epub+zip";
    r.body = kEpubBytes;
    return r;
  }
  if (p == "/search.php") {
    r.body =
        "<html><body><table><tr><td>ID</td><td>Author(s)</td><td>Title</td><td>Publisher</td>"
        "<td>Year</td><td>Pages</td><td>Language</td><td>Size</td><td>Extension</td>"
        "<td>Mirrors</td></tr>"
        "<tr><td>1</td><td>A. Writer</td><td><a href=\"book/index.php?md5="
        "0123456789abcdef0123456789abcdef\">Fic about " +
        catalogues::html_escape(request.param("req")) + "</a></td><td>-</td><td>2020</td>"
        "<td>10</td><td>English</td><td>12 Kb</td><td>epub</td>"
        "<td><a href=\"/get.php?md5=0123456789abcdef0123456789abcdef\">[1]</a></td></tr>"
        "</table></body></html>";
    return r;
  }
  if (p == "/get.php") {
    r.content_type = "application/epub+zip";
    r.body = kEpubBytes;
    return r;
  }
  r.status = 404;
  r.body = "no";
  return r;
}

// One request over a real socket, the way the browser does it.
std::string fetch(int port, const std::string& method, const std::string& target,
                  const std::string& form = "") {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(fd);
    return "";
  }
  std::string request = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  if (!form.empty()) {
    request += "Content-Type: application/x-www-form-urlencoded\r\n";
    request += format("Content-Length: %zu\r\n", form.size());
  }
  request += "\r\n" + form;
  if (write(fd, request.data(), request.size()) < 0) {
    close(fd);
    return "";
  }
  std::string out;
  char buffer[4096];
  ssize_t n;
  while ((n = read(fd, buffer, sizeof(buffer))) > 0) out.append(buffer, (size_t)n);
  close(fd);
  return out;
}

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string link_after(const std::string& html, const std::string& marker) {
  size_t at = html.find(marker);
  if (at == std::string::npos) return "";
  size_t start = html.rfind("href=\"", at);
  if (start == std::string::npos) return "";
  start += 6;
  size_t end = html.find('"', start);
  std::string href = html.substr(start, end - start);
  return replace_all(replace_all(href, "&amp;", "&"), "&#39;", "'");
}

}  // namespace

int main() {
  log_init("", LogLevel::Error);

  char tmpl[] = "/tmp/catalogues-test-XXXXXX";
  std::string root = mkdtemp(tmpl);
  Paths p;
  p.onboard = root;
  p.data = root + "/.adds/catalogues";
  p.install = root + "/install";
  set_paths(p);
  fs::mkdir_p(p.data);

  volatile bool stop = false;
  std::thread fake([&] { catalogues::serve("127.0.0.1", kFakePort, fake_server, &stop); });
  std::thread app([&] { catalogues::serve("127.0.0.1", kAppPort, catalogues::handle, &stop); });
  for (int i = 0; i < 100 && !(catalogues::ping(kAppPort)); ++i) usleep(50000);
  CHECK(catalogues::ping(kAppPort));
  CHECK(!catalogues::ping(kAppPort + 7));

  // ------------------------------------------------ first run: the file
  std::string home = fetch(kAppPort, "GET", "/");
  CHECK(has(home, "HTTP/1.1 200"));
  CHECK(has(home, "text/html"));
  CHECK(fs::exists(p.data + "/catalogues.txt"));   // written on first use
  CHECK(has(home, "Project Gutenberg"));            // and it starts with the public ones
  CHECK(has(home, "How this works"));

  // The reader edits the file over USB; the next page shows the change.
  fs::write_file_atomic(p.data + "/catalogues.txt",
                        "# mine\n"
                        "Fake Library | " + fake_base() + "/opds\n"
                        "Fic | " + fake_base() + "/search.php?req={searchTerms}\n");
  home = fetch(kAppPort, "GET", "/");
  CHECK(has(home, "Fake Library"));
  CHECK(has(home, "href=\"/c/0\""));
  CHECK(has(home, "href=\"/c/1\""));
  CHECK(!has(home, "Gutenberg"));
  CHECK(has(home, "<span class=\"g\">search</span>"));   // the search-only one is marked

  // The menu entry that jumps to a named catalogue.
  std::string jump = fetch(kAppPort, "GET", "/?open=fake%20library");
  CHECK(has(jump, "HTTP/1.1 302"));
  CHECK(has(jump, "Location: /c/0\r\n"));
  std::string missing = fetch(kAppPort, "GET", "/?open=Shelfmark");
  CHECK(has(missing, "HTTP/1.1 200"));
  CHECK(has(missing, "no catalogue called <b>Shelfmark</b>"));

  // ------------------------------------------------ browsing a feed
  std::string feed = fetch(kAppPort, "GET", "/c/0");
  CHECK(has(feed, "HTTP/1.1 200"));
  CHECK(has(feed, "<h1>Fake Library</h1>"));
  CHECK(has(feed, "Fiction"));
  CHECK(has(feed, "class=\"row nav\""));
  CHECK(has(feed, "Moby Dick"));
  CHECK(has(feed, "Herman Melville"));
  CHECK(has(feed, "<span class=\"g\">EPUB</span>"));
  CHECK(has(feed, "<img src=\"" + fake_base() + "/covers/moby.jpg\""));
  CHECK(has(feed, "action=\"/c/0/search\""));   // the feed advertises a search
  CHECK(has(feed, "<script>"));                  // the busy-button script rides along

  std::string folder = link_after(feed, "Fiction");
  CHECK(starts_with(folder, "/c/0?url="));
  std::string sub = fetch(kAppPort, "GET", folder);
  CHECK(has(sub, "<h1>Fiction</h1>"));
  CHECK(has(sub, "Tom &amp; Jerry"));
  CHECK(has(sub, ">More<"));                                  // rel=next
  CHECK(has(sub, "class=\"back\" href=\"/c/0?url="));       // rel=up

  // ------------------------------------------------ a book, then the download
  std::string book_href = link_after(feed, "Moby Dick");
  CHECK(starts_with(book_href, "/c/0/book?"));
  std::string book = fetch(kAppPort, "GET", book_href);
  CHECK(has(book, "HTTP/1.1 200"));
  CHECK(has(book, "<h2>Moby Dick</h2>"));
  CHECK(has(book, "method=\"post\" action=\"/c/0/download\""));
  CHECK(has(book, "name=\"url\" value=\"" + fake_base() + "/files/moby.epub\""));
  CHECK(has(book, "name=\"back\" value=\"/c/0\""));
  CHECK(has(book, ">Download<"));

  std::string form = "title=Moby+Dick&author=Herman+Melville&url=" +
                     url_encode(fake_base() + "/files/moby.epub") +
                     "&type=application%2Fepub%2Bzip&md5=&ext=&back=%2Fc%2F0";
  std::string done = fetch(kAppPort, "POST", "/c/0/download", form);
  CHECK(has(done, "HTTP/1.1 200"));
  CHECK(has(done, "<h1>Downloaded</h1>"));
  CHECK(has(done, "Downloads/"));
  CHECK(has(done, "class=\"good\""));
  std::vector<fs::Entry> saved = fs::list_dir(root + "/Downloads");
  CHECK(saved.size() == 1);
  if (!saved.empty()) {
    CHECK(has(saved[0].name, "Moby Dick"));
    CHECK(ends_with(saved[0].name, ".epub"));
    std::string bytes;
    fs::read_file(saved[0].path, bytes);
    CHECK(bytes == kEpubBytes);
  }
  std::string downloads = fetch(kAppPort, "GET", "/downloads");
  CHECK(has(downloads, "Moby Dick"));
  // A GET to the download address does nothing but go back.
  CHECK(has(fetch(kAppPort, "GET", "/c/0/download"), "HTTP/1.1 302"));

  // ------------------------------------------------ OPDS search
  std::string results = fetch(kAppPort, "GET", "/c/0/search?q=whales+%26+ships");
  CHECK(has(results, "HTTP/1.1 200"));
  CHECK(has(results, "Found: whales &amp; ships"));
  CHECK(has(results, "value=\"whales &amp; ships\""));      // the box keeps the words
  std::string blank = fetch(kAppPort, "GET", "/c/0/search?q=");
  CHECK(has(blank, "HTTP/1.1 200"));
  CHECK(has(blank, "action=\"/c/0/search\""));

  // ------------------------------------------------ the search-format server
  std::string fic = fetch(kAppPort, "GET", "/c/1");
  CHECK(has(fic, "HTTP/1.1 200"));
  CHECK(has(fic, "action=\"/c/1/search\""));
  CHECK(has(fic, "searched rather than browsed"));
  std::string fic_results = fetch(kAppPort, "GET", "/c/1/search?q=dragons");
  CHECK(has(fic_results, "Fic about dragons"));
  CHECK(has(fic_results, "A. Writer"));
  CHECK(has(fic_results, "2020"));
  CHECK(has(fic_results, "<span class=\"g\">EPUB</span>"));
  std::string fic_book_href = link_after(fic_results, "Fic about dragons");
  CHECK(has(fic_book_href, "md5=0123456789abcdef0123456789abcdef"));
  std::string fic_book = fetch(kAppPort, "GET", fic_book_href);
  CHECK(has(fic_book, "name=\"md5\" value=\"0123456789abcdef0123456789abcdef\""));
  std::string fic_form = "title=Fic+about+dragons&author=A.+Writer&url=&type=&md5="
                         "0123456789abcdef0123456789abcdef&ext=epub&back=%2Fc%2F1";
  std::string fic_done = fetch(kAppPort, "POST", "/c/1/download", fic_form);
  CHECK(has(fic_done, "<h1>Downloaded</h1>"));
  saved = fs::list_dir(root + "/Downloads");
  CHECK(saved.size() == 2);

  // ------------------------------------------------ the edges
  CHECK(has(fetch(kAppPort, "GET", "/c/9"), "HTTP/1.1 404"));
  CHECK(has(fetch(kAppPort, "GET", "/c/x"), "HTTP/1.1 404"));
  CHECK(has(fetch(kAppPort, "GET", "/nothing"), "HTTP/1.1 404"));
  CHECK(has(fetch(kAppPort, "GET", "/help"), "catalogues.txt"));
  std::string status = fetch(kAppPort, "GET", "/status");
  CHECK(has(status, "application/json"));
  CHECK(has(status, "\"ok\":true"));
  // A feed that is down is a message, not a blank page.
  fs::write_file_atomic(p.data + "/catalogues.txt", "Gone | http://127.0.0.1:1/opds\n");
  std::string gone = fetch(kAppPort, "GET", "/c/0");
  CHECK(has(gone, "HTTP/1.1 502"));
  CHECK(has(gone, "class=\"bad\""));
  CHECK(has(gone, "All catalogues"));

  // ------------------------------------------------ the pieces
  {
    using namespace catalogues;
    CHECK(catalogues::url_decode("a+b%20c%2F") == "a b c/");
    CHECK(html_escape("<a href='x'>&\"") == "&lt;a href=&#39;x&#39;&gt;&amp;&quot;");
    CHECK(json_escape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
    auto form_fields = parse_form("title=Moby+Dick&x=&y=%26");
    CHECK(form_fields["title"] == "Moby Dick");
    CHECK(form_fields["x"].empty());
    CHECK(form_fields["y"] == "&");
    Request r;
    r.query["a"] = "q";
    r.form["a"] = "f";
    CHECK(r.param("a") == "f");     // the form wins
    CHECK(r.param("b").empty());
    // A bad request is answered, not hung up on.
    CHECK(has(fetch(kAppPort, "GET", ""), "HTTP/1.1 400"));
  }


  // ------------------------------------------------ Shelfmark: its own file
  // Shelfmark is a search engine with its own sources file, separate from
  // catalogues.txt. Point it at the fake search-format server.
  fs::write_file_atomic(p.data + "/shelfmark.txt",
                        "# mine\n"
                        "My Fic | " + fake_base() + "/search.php?req={searchTerms}\n");
  std::string sm = fetch(kAppPort, "GET", "/shelfmark");
  CHECK(has(sm, "HTTP/1.1 200"));
  CHECK(has(sm, "<h1>Shelfmark</h1>"));
  CHECK(has(sm, "#6a1b9a"));                        // its own colour, not the blue
  CHECK(has(sm, "action=\"/shelfmark/search\""));
  CHECK(has(sm, "My Fic"));
  // A search hits the source and lists results, each a Shelfmark book link.
  std::string smr = fetch(kAppPort, "GET", "/shelfmark/search?q=dragons");
  CHECK(has(smr, "HTTP/1.1 200"));
  CHECK(has(smr, "Fic about dragons"));
  CHECK(has(smr, "/shelfmark/book?s=0"));
  CHECK(has(smr, "#6a1b9a"));
  std::string smbook_href = link_after(smr, "Fic about dragons");
  CHECK(starts_with(smbook_href, "/shelfmark/book?s=0"));
  std::string smbook = fetch(kAppPort, "GET", smbook_href);
  CHECK(has(smbook, "action=\"/shelfmark/download\""));
  CHECK(has(smbook, "name=\"s\" value=\"0\""));
  CHECK(has(smbook, "name=\"md5\" value=\"0123456789abcdef0123456789abcdef\""));
  // And a download lands a file, via Shelfmark's own route.
  std::string smform = "s=0&title=Fic+about+dragons&author=A.+Writer&url=&type=&md5="
                       "0123456789abcdef0123456789abcdef&ext=epub&back=%2Fshelfmark";
  std::string smdone = fetch(kAppPort, "POST", "/shelfmark/download", smform);
  CHECK(has(smdone, "HTTP/1.1 200"));
  CHECK(has(smdone, "<h1>Downloaded</h1>"));
  CHECK(has(smdone, "class=\"good\""));
  // Shelfmark never touched catalogues.txt.
  {
    std::string cats;
    fs::read_file(p.data + "/catalogues.txt", cats);
    CHECK(!has(cats, "My Fic"));
  }
  // With no shelfmark.txt, Shelfmark says how to set up its own file.
  fs::remove_file(p.data + "/shelfmark.txt");
  std::string sm_empty = fetch(kAppPort, "GET", "/shelfmark");
  CHECK(has(sm_empty, "No search servers yet"));
  CHECK(has(sm_empty, "shelfmark.txt"));

  stop = true;
  fake.join();
  app.join();
  fs::remove_tree(root);

  if (failures == 0) printf("test_catalogues: all passed\n");
  return failures == 0 ? 0 : 1;
}
