// Builds a small EPUB in memory, opens it through the real Book/Layout
// stack, paginates it and renders pages to PNG. This is the closest thing
// to a reading smoke test that can run off-device.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "app/settings.h"
#include "core/fs.h"
#include "core/log.h"
#include "net/discover.h"
#include "net/libgen.h"
#include "net/opds.h"
#include "epub/book.h"
#include "core/str.h"
#include "epub/layout.h"
#include "gfx/canvas.h"
#include "gfx/font.h"
#include "miniz.h"

using namespace ck;

static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                     \
    }                                                                 \
  } while (0)

namespace {

const char* kContainer =
    "<?xml version=\"1.0\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "  <rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
    "media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
    "</container>\n";

const char* kOpf =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">\n"
    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
    "    <dc:title>The Colour of Ink</dc:title>\n"
    "    <dc:creator>A. Tester</dc:creator>\n"
    "    <dc:language>en</dc:language>\n"
    "    <dc:identifier id=\"id\">urn:uuid:crosskobo-test</dc:identifier>\n"
    "    <meta name=\"calibre:series\" content=\"E-Ink Tales\"/>\n"
    "    <meta name=\"calibre:series_index\" content=\"2\"/>\n"
    "  </metadata>\n"
    "  <manifest>\n"
    "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
    "properties=\"nav\"/>\n"
    "    <item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n"
    "    <item id=\"c1\" href=\"chap1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
    "    <item id=\"c2\" href=\"chap2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
    "    <item id=\"cover\" href=\"cover.png\" media-type=\"image/png\" "
    "properties=\"cover-image\"/>\n"
    "    <item id=\"fig\" href=\"figure.png\" media-type=\"image/png\"/>\n"
    "  </manifest>\n"
    "  <spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine>\n"
    "</package>\n";

const char* kNav =
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>\n"
    "<nav epub:type=\"toc\"><ol>\n"
    "  <li><a href=\"chap1.xhtml\">A Beginning</a></li>\n"
    "  <li><a href=\"chap2.xhtml\">Colour Plates</a>\n"
    "     <ol><li><a href=\"chap2.xhtml#plate\">The Plate</a></li></ol></li>\n"
    "</ol></nav></body></html>\n";

const char* kCss =
    "p { text-indent: 1.2em; margin-bottom: 0.2em; }\n"
    "h1 { font-size: 1.6em; text-align: center; }\n"
    ".drop { font-weight: bold; color: #D01414; }\n"
    ".hidden { display: none; }\n"
    "blockquote { margin-left: 2em; font-style: italic; }\n";

std::string long_paragraph(int n) {
  static const char* kWords[] = {
      "paper",   "ink",     "colour",     "kaleido",  "waveform",  "refresh",
      "stylus",  "margin",  "typography", "reading",  "chapter",   "pagination",
      "library", "notebook","handwriting","greyscale","saturation","dithering"};
  std::string out;
  for (int i = 0; i < n; ++i) {
    if (i) out += ' ';
    out += kWords[(i * 7 + 3) % 18];
    if (i % 11 == 10) out += ',';
  }
  out += '.';
  return out;
}

std::string chapter_one() {
  std::string html =
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
      "<link rel=\"stylesheet\" href=\"style.css\"/></head><body>\n"
      "<h1 id=\"start\">A Beginning</h1>\n"
      "<p class=\"drop\">The first paragraph is red and bold because the book said so.</p>\n";
  for (int i = 0; i < 6; ++i) {
    html += "<p>" + long_paragraph(70 + i * 9) +
            " Some <em>emphasis</em>, some <strong>strength</strong>, a "
            "<a href=\"chap2.xhtml#plate\">cross reference</a> and a footnote"
            "<a href=\"chap2.xhtml#note1\"><sup>1</sup></a>.</p>\n";
  }
  html +=
      "<blockquote><p>A quotation, indented and italic.</p></blockquote>\n"
      "<ul><li>First item</li><li>Second item with rather more text in it so that it "
      "has to wrap onto a second line at any sensible font size</li></ul>\n"
      "<ol><li>Numbered one</li><li>Numbered two</li></ol>\n"
      "<p class=\"hidden\">This must never be rendered.</p>\n"
      "<hr/>\n"
      "<p>" + long_paragraph(120) + "</p>\n"
      "</body></html>\n";
  return html;
}

std::string chapter_two() {
  return std::string(
             "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
             "<link rel=\"stylesheet\" href=\"style.css\"/></head><body>\n"
             "<h1>Colour Plates</h1>\n"
             "<p id=\"plate\">A colour figure follows.</p>\n"
             "<p><img src=\"figure.png\" alt=\"figure\"/></p>\n"
             "<p id=\"note1\">1. The footnote target.</p>\n"
             "<p>") +
         long_paragraph(90) + "</p>\n</body></html>\n";
}

// A small colour PNG, encoded with the same writer the app uses.
std::string make_png(int w, int h, bool gradient) {
  Canvas c(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      Color px = gradient ? Color((uint8_t)(x * 255 / w), (uint8_t)(y * 255 / h),
                                  (uint8_t)(200 - x * 120 / w))
                          : Color::rgb(0x1146C8);
      c.set_pixel(x, y, px);
    }
  }
  std::string out;
  c.encode_png(out);
  return out;
}

bool build_epub(const std::string& path) {
  fs::remove_file(path);
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_writer_init_file(&zip, path.c_str(), 0)) return false;
  auto add = [&](const char* name, const std::string& data, bool store) {
    return mz_zip_writer_add_mem(&zip, name, data.data(), data.size(),
                                 store ? MZ_NO_COMPRESSION : MZ_DEFAULT_COMPRESSION) != 0;
  };
  bool ok = true;
  ok &= add("mimetype", "application/epub+zip", true);
  ok &= add("META-INF/container.xml", kContainer, false);
  ok &= add("OEBPS/content.opf", kOpf, false);
  ok &= add("OEBPS/nav.xhtml", kNav, false);
  ok &= add("OEBPS/style.css", kCss, false);
  ok &= add("OEBPS/chap1.xhtml", chapter_one(), false);
  ok &= add("OEBPS/chap2.xhtml", chapter_two(), false);
  ok &= add("OEBPS/cover.png", make_png(300, 450, false), false);
  ok &= add("OEBPS/figure.png", make_png(480, 320, true), false);
  ok &= mz_zip_writer_finalize_archive(&zip) != 0;
  mz_zip_writer_end(&zip);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  log_init("", LogLevel::Warn);
  std::string assets = argc > 1 ? argv[1] : "assets/fonts";
  FontManager::instance().scan({assets});

  const std::string epub_path = "out/fixture.epub";
  fs::mkdir_p("out");
  CHECK(build_epub(epub_path));

  Book book;
  CHECK(book.open(epub_path));
  CHECK(book.format() == Book::Format::Epub);
  CHECK(book.metadata().title == "The Colour of Ink");
  CHECK(book.metadata().author == "A. Tester");
  CHECK(book.metadata().series == "E-Ink Tales");
  CHECK(book.metadata().series_index == 2);
  CHECK(book.spine().size() == 2);
  CHECK(book.toc().size() == 3);
  if (book.toc().size() == 3) {
    CHECK(book.toc()[0].title == "A Beginning");
    CHECK(book.toc()[0].spine_index == 0);
    CHECK(book.toc()[2].spine_index == 1);
    CHECK(book.toc()[2].depth == 1);
  }
  CHECK(book.has_cover());
  if (book.cover()) {
    CHECK(book.cover()->width() == 300);
    CHECK(book.cover()->height() == 450);
  }

  // Libra Colour geometry.
  const int W = 1264, H = 1680;
  LayoutParams params;
  params.content = Rect(60, 90, W - 120, H - 150);
  params.font_family = "Literata";
  params.font_px = 40;
  params.line_spacing = 145;
  params.align = Alignment::Justify;
  params.dpi = 300;

  Layout layout(book, params);
  Document doc = layout.parse(0);
  CHECK(doc.blocks.size() > 10);
  CHECK(!doc.links.empty());
  CHECK(doc.anchors.count("start") == 1);

  // display:none must be dropped.
  bool leaked_hidden = false;
  for (const Block& b : doc.blocks) {
    for (const auto& f : b.frags) {
      if (f.first.find("must never be rendered") != std::string::npos) leaked_hidden = true;
    }
  }
  CHECK(!leaked_hidden);

  // The book's own CSS colour must survive into the runs.
  bool found_red = false;
  for (const Block& b : doc.blocks) {
    for (const auto& f : b.frags) {
      if (f.second.has_color && f.second.color.r > 150 && f.second.color.g < 80) found_red = true;
    }
  }
  CHECK(found_red);

  std::vector<Page> pages = layout.paginate(doc);
  CHECK(pages.size() >= 3);
  printf("chapter 1: %zu blocks -> %zu pages\n", doc.blocks.size(), pages.size());

  // Pages must be strictly ordered and cover the chapter without gaps.
  for (size_t i = 1; i < pages.size(); ++i) {
    bool advanced = pages[i].start_block > pages[i - 1].start_block ||
                    (pages[i].start_block == pages[i - 1].start_block &&
                     (pages[i].start_frag > pages[i - 1].start_frag ||
                      pages[i].start_offset > pages[i - 1].start_offset));
    CHECK(advanced);
  }
  // Resume must land on the page that contains the position.
  for (size_t i = 0; i < pages.size(); ++i) {
    int found = Layout::page_for_position(pages, pages[i].start_block, pages[i].start_frag,
                                          pages[i].start_offset);
    CHECK(found == (int)i);
  }
  // No line may overflow the content box.
  for (const Page& p : pages) {
    for (const Line& l : p.lines) {
      CHECK(l.y + l.height <= params.content.h + 2);
      for (const Run& r : l.runs) CHECK(r.x + r.width <= params.content.w + 4);
    }
  }

  Canvas screen(W, H);
  for (size_t i = 0; i < pages.size() && i < 2; ++i) {
    screen.clear(Color::gray(255));
    std::vector<std::pair<Rect, int>> links;
    layout.draw(screen, pages[i], doc, &links);
    if (i == 0) CHECK(!links.empty());
    int painted = 0;
    for (int y = 0; y < H; y += 2) {
      for (int x = 0; x < W; x += 2) {
        if (screen.get_pixel(x, y) != Color::gray(255)) ++painted;
      }
    }
    CHECK(painted > 3000);
    CHECK(screen.save_png(format("out/page%zu.png", i + 1)));
  }

  // Chapter two has the colour figure.
  Document doc2 = layout.parse(1);
  std::vector<Page> pages2 = layout.paginate(doc2);
  CHECK(!pages2.empty());
  bool has_image_line = false;
  for (const Page& p : pages2) {
    for (const Line& l : p.lines) {
      if (!l.image_href.empty()) has_image_line = true;
    }
  }
  CHECK(has_image_line);
  screen.clear(Color::gray(255));
  layout.draw(screen, pages2.front(), doc2, nullptr);
  CHECK(screen.save_png("out/page_colour.png"));
  // The rendered figure must still be in colour.
  bool colourful = false;
  for (int y = 0; y < H && !colourful; ++y) {
    for (int x = 0; x < W; ++x) {
      Color c = screen.get_pixel(x, y);
      if (std::max(c.r, std::max(c.g, c.b)) - std::min(c.r, std::min(c.g, c.b)) > 40) {
        colourful = true;
        break;
      }
    }
  }
  CHECK(colourful);


  // OPDS catalogues: the Atom dialect every self-hosted library speaks.
  // Real feeds mix namespace prefixes, relative links and several
  // acquisition formats per entry, so parse a sample shaped like one.
  {
    const char* kFeed =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<feed xmlns=\"http://www.w3.org/2005/Atom\" "
        "xmlns:opds=\"http://opds-spec.org/2010/catalog\">\n"
        "  <title>Shelfmark</title>\n"
        "  <link rel=\"search\" type=\"application/atom+xml\" "
        "href=\"/opds/search?q={searchTerms}\"/>\n"
        "  <link rel=\"next\" href=\"/opds/all?page=2\"/>\n"
        "  <entry>\n"
        "    <title>Fiction</title>\n"
        "    <link type=\"application/atom+xml;profile=opds-catalog\" href=\"fiction\"/>\n"
        "  </entry>\n"
        "  <entry>\n"
        "    <title>The Colour of Ink</title>\n"
        "    <author><name>A. Tester</name></author>\n"
        "    <summary>A book about pigment.</summary>\n"
        "    <link rel=\"http://opds-spec.org/image/thumbnail\" href=\"/covers/7.jpg\"/>\n"
        "    <link rel=\"http://opds-spec.org/acquisition\" type=\"application/pdf\" "
        "href=\"/get/7.pdf\"/>\n"
        "    <link rel=\"http://opds-spec.org/acquisition\" "
        "type=\"application/epub+zip\" length=\"4096\" href=\"/get/7.epub\"/>\n"
        "  </entry>\n"
        "</feed>\n";
    OpdsFeed feed;
    CHECK(parse_opds(kFeed, "https://books.example.org:8443/opds/root", feed));
    CHECK(feed.title == "Shelfmark");
    CHECK(feed.entries.size() == 2);
    CHECK(feed.next_url == "https://books.example.org:8443/opds/all?page=2");
    if (feed.entries.size() == 2) {
      // Navigation entry: a relative href resolves against the feed's directory.
      CHECK(feed.entries[0].is_navigation());
      CHECK(feed.entries[0].feed_url == "https://books.example.org:8443/opds/fiction");
      // Acquisition entry: EPUB wins over the PDF listed before it.
      const OpdsEntry& book = feed.entries[1];
      CHECK(!book.is_navigation());
      CHECK(book.author == "A. Tester");
      CHECK(book.summary == "A book about pigment.");
      CHECK(book.download_url == "https://books.example.org:8443/get/7.epub");
      CHECK(book.size == 4096);
      CHECK(book.cover_url == "https://books.example.org:8443/covers/7.jpg");
      CHECK(book.filename() == "A. Tester - The Colour of Ink.epub");
    }
    std::string search = opds_search_url(feed.search_url, "colour ink");
    CHECK(search == "https://books.example.org:8443/opds/search?q=colour%20ink");
    // A catalogue that advertises a bare endpoint still gets a query.
    CHECK(opds_search_url("https://x/opds/find", "abc") == "https://x/opds/find?q=abc");
  }

  // The search format Library Genesis popularised, which self-hosted
  // catalogue software inherits by forking it. Every fork moves the columns
  // around, so the parser identifies them by what they hold - and that is
  // exactly the part worth testing.
  {
    const char* kMd5a = "0123456789abcdef0123456789abcdef";
    const char* kMd5b = "fedcba9876543210fedcba9876543210";
    std::string html =
        "<table class='c'><tr><th>ID</th><th>Author</th><th>Title</th>"
        "<th>Publisher</th><th>Year</th><th>Pages</th><th>Language</th>"
        "<th>Size</th><th>Extension</th><th>Mirrors</th></tr>\n"
        "<tr valign=top><td>1207</td><td><a href='author.php?id=9'>A. Tester</a></td>"
        "<td width=500><a href='book/index.php?md5=" + std::string(kMd5a) +
        "'>The Colour of Ink</a></td><td>Self</td><td>2024</td><td>312</td>"
        "<td>English</td><td>1.4 Mb</td><td>epub</td>"
        "<td><a href='/main/" + std::string(kMd5a) + "'>[1]</a></td></tr>\n"
        "<tr valign=top><td>1208</td><td>B. Writer</td>"
        "<td width=500><a href='book/index.php?md5=" + std::string(kMd5b) +
        "'>A Short Walk</a></td><td>Self</td><td>2019</td><td>88</td>"
        "<td>English</td><td>755 Kb</td><td>pdf</td>"
        "<td><a href='/main/" + std::string(kMd5b) + "'>[1]</a></td></tr>\n"
        "</table>";
    std::vector<SearchResult> results;
    parse_libgen_html(html, results);
    CHECK(results.size() == 2);
    if (results.size() == 2) {
      CHECK(results[0].title == "The Colour of Ink");
      CHECK(results[0].author == "A. Tester");
      CHECK(results[0].extension == "epub");
      CHECK(results[0].size_text == "1.4 Mb");
      CHECK(results[0].year == "2024");
      CHECK(results[0].md5 == kMd5a);
      CHECK(results[0].filename() == "A. Tester - The Colour of Ink.epub");
      CHECK(results[1].title == "A Short Walk");
      CHECK(results[1].extension == "pdf");
      CHECK(results[1].md5 == kMd5b);
    }

    // The JSON form, with the key case forks disagree about and a byte
    // count rather than a written size.
    std::string json =
        "[{\"MD5\":\"" + std::string(kMd5a) + "\",\"Title\":\"Notes on Colour\","
        "\"Author\":\"C. Painter\",\"Extension\":\"EPUB\",\"FileSize\":\"2097152\","
        "\"Year\":\"2021\"}]";
    std::vector<SearchResult> from_json;
    CHECK(parse_libgen_json(json, from_json));
    CHECK(from_json.size() == 1);
    if (from_json.size() == 1) {
      CHECK(from_json[0].title == "Notes on Colour");
      CHECK(from_json[0].extension == "epub");
      CHECK(from_json[0].md5 == kMd5a);
      CHECK(from_json[0].size_text.find("2") != std::string::npos);
    }
    // An object wrapping the array is also seen in the wild.
    std::vector<SearchResult> wrapped;
    CHECK(parse_libgen_json("{\"data\":" + json + "}", wrapped));
    CHECK(wrapped.size() == 1);

    // Picking the file id out of a link, in the shapes servers use.
    CHECK(md5_from_link("book/index.php?md5=" + std::string(kMd5a)) == kMd5a);
    CHECK(md5_from_link("http://host/main/" + std::string(kMd5b)) == kMd5b);
    CHECK(md5_from_link("/get.php?md5=" + std::string(kMd5a) + "&key=xyz") == kMd5a);
    CHECK(md5_from_link("author.php?id=9").empty());
    CHECK(md5_from_link("/main/not-a-hash").empty());

    // The mirror page: the link labelled GET wins over anything else.
    std::string page =
        "<html><body><a href='/index.php'>Home</a>"
        "<a href='/covers/x.jpg'>cover</a>"
        "<h2><a href='/get.php?md5=" + std::string(kMd5a) + "&key=abc'>GET</a></h2>"
        "<a href='/other.epub'>mirror 2</a></body></html>";
    std::string link = direct_link_from_page(page, "http://host:8080");
    CHECK(link == "http://host:8080/get.php?md5=" + std::string(kMd5a) + "&key=abc");
    // With no GET link, a file-shaped one will do.
    CHECK(direct_link_from_page("<a href='/books/x.epub'>dl</a>", "http://h") ==
          "http://h/books/x.epub");
    CHECK(direct_link_from_page("<a href='/about'>about</a>", "http://h").empty());

    // Which addresses are this format, and which are not.
    CHECK(looks_like_libgen_endpoint("http://host/search.php?req={searchTerms}"));
    CHECK(looks_like_libgen_endpoint("http://host:8080/json.php"));
    CHECK(!looks_like_libgen_endpoint("http://host/opds"));
    CHECK(!looks_like_libgen_endpoint("https://standardebooks.org/feeds/opds"));
  }

  // Discovery probes a fixed set of ports and paths; a typo in either is
  // the difference between finding a server and sweeping for nothing.
  {
    const std::vector<int>& ports = discovery_ports();
    CHECK(std::find(ports.begin(), ports.end(), 8083) != ports.end());   // Calibre-Web
    CHECK(std::find(ports.begin(), ports.end(), 5000) != ports.end());   // Kavita
    CHECK(std::find(ports.begin(), ports.end(), 25600) != ports.end());  // Komga
    const std::vector<std::string>& paths = discovery_paths();
    CHECK(std::find(paths.begin(), paths.end(), "/opds") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "/api/opds") != paths.end());
    for (const std::string& path : paths) CHECK(!path.empty() && path[0] == '/');
  }

  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
