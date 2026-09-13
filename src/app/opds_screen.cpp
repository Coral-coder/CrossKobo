// Catalogue browsing. A saved list of OPDS catalogues, then a feed browser
// that walks navigation entries and downloads books straight onto the
// drive, where the library picks them up like anything else copied over
// USB.
#include <algorithm>
#include <memory>
#include <string>

#include "app/app.h"
#include "app/home.h"
#include "app/settings.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "library/library.h"
#include "net/http.h"
#include "net/discover.h"
#include "net/libgen.h"
#include "net/opds.h"
#include "platform/net.h"
#include "ui/keyboard.h"
#include "ui/list_view.h"
#include "ui/theme.h"

namespace ck {

// Opens a saved catalogue with whichever client its format needs; defined
// below, once both browsers exist.
ViewPtr open_catalogue(const Settings::Catalogue& c);

namespace {

enum BrowseId {
  kEntryBase = 1,
  kSearch = -10,
  kNextPage = -11,
  kReload = -12,
};

std::string download_dir() {
  const std::string& folder = settings().catalogue_folder;
  if (folder.empty()) return paths().onboard;
  return paths().onboard + "/" + folder;
}

// One feed. Fetching is synchronous and modal: a catalogue answers in a
// second or two on a home network, and a half-drawn list on e-ink is worse
// than a brief wait with a message on screen.
class OpdsBrowser : public ListView {
 public:
  OpdsBrowser(std::string title, std::string url, const Settings::Catalogue& credentials)
      : ListView(std::move(title), {}, nullptr), url_(std::move(url)), auth_(credentials) {
    set_empty_message("This part of the catalogue is empty.");
    on_select_ = [this](int id) { activate(id); };
  }

  void on_show() override {
    if (loaded_) return;
    if (auth_.search_only()) {
      // Nothing to browse: this catalogue is a search endpoint, so ask for
      // the query straight away.
      loaded_ = true;
      feed_.search_url = auth_.search;
      rebuild();
      activate(kSearch);
      return;
    }
    load(url_);
  }

 private:
  void load(const std::string& url) {
    App& app = App::instance();
    if (!Net::instance().connected()) {
      app.show_message("Catalogue", "Connect to Wi-Fi first: Settings has a Wi-Fi entry.");
      app.pop();
      return;
    }
    set_status_line("Loading…");
    app.render_now();

    std::string error;
    OpdsFeed feed;
    if (!fetch_opds(url, auth_.user, auth_.password, feed, error)) {
      set_status_line("");
      app.show_message("Catalogue", error);
      if (!loaded_) app.pop();
      return;
    }
    loaded_ = true;
    url_ = url;
    feed_ = feed;
    // A search address saved with the catalogue wins over whatever the feed
    // advertises: it is the one the reader chose.
    if (!auth_.search.empty()) feed_.search_url = auth_.search;
    if (!feed_.title.empty() && title_.empty()) set_title(feed_.title);
    rebuild();
  }

  void rebuild() {
    std::vector<Item> items;
    for (size_t i = 0; i < feed_.entries.size(); ++i) {
      const OpdsEntry& entry = feed_.entries[i];
      Item item;
      item.id = kEntryBase + (int)i;
      item.row.title = entry.title.empty() ? "(untitled)" : entry.title;
      if (entry.is_navigation()) {
        item.row.trailing = "›";
      } else {
        item.row.subtitle = entry.author;
        if (entry.size > 0) item.row.trailing = human_size((uint64_t)entry.size);
      }
      items.push_back(item);
    }
    std::vector<std::pair<std::string, int>> actions;
    if (!feed_.search_url.empty()) actions.push_back({"Search", kSearch});
    if (!feed_.next_url.empty()) actions.push_back({"More", kNextPage});
    actions.push_back({"Reload", kReload});
    set_actions(actions);
    set_status_line(format("%zu item%s", feed_.entries.size(),
                           feed_.entries.size() == 1 ? "" : "s"));
    set_items(std::move(items));
  }

  void activate(int id) {
    if (id == kReload) {
      load(url_);
      return;
    }
    if (id == kNextPage) {
      load(feed_.next_url);
      return;
    }
    if (id == kSearch) {
      std::string tmpl = feed_.search_url;
      Settings::Catalogue auth = auth_;
      std::string name = title_;
      App::instance().push(ViewPtr(new KeyboardView(
          "Search the catalogue", "", [tmpl, auth, name](const std::string& text, bool ok) {
            App::instance().pop();
            if (!ok || trim(text).empty()) return;
            std::string url = opds_search_url(tmpl, trim(text));
            App::instance().push(
                ViewPtr(new OpdsBrowser("Results: " + trim(text), url, auth)));
          })));
      return;
    }
    size_t index = (size_t)(id - kEntryBase);
    if (index >= feed_.entries.size()) return;
    const OpdsEntry& entry = feed_.entries[index];
    if (entry.is_navigation()) {
      App::instance().push(ViewPtr(new OpdsBrowser(entry.title, entry.feed_url, auth_)));
      return;
    }
    offer_download(entry);
  }

  void offer_download(const OpdsEntry& entry) {
    App& app = App::instance();
    std::string details;
    if (!entry.author.empty()) details += entry.author + "\n\n";
    if (!entry.summary.empty()) {
      details += entry.summary.size() > 400 ? entry.summary.substr(0, 400) + "…"
                                            : entry.summary;
      details += "\n\n";
    }
    details += format("Saves to %s as %s", settings().catalogue_folder.c_str(),
                      entry.filename().c_str());
    if (!app.confirm(entry.title, details, "Download", "Back")) return;

    set_status_line("Downloading…");
    app.render_now();
    std::string path;
    std::string error;
    if (!download_opds_entry(entry, download_dir(), auth_.user, auth_.password, path, error)) {
      set_status_line("");
      app.show_message("Download failed", error);
      return;
    }
    set_status_line("");
    if (app.confirm("Downloaded", entry.title + "\n\nSaved to " + settings().catalogue_folder +
                                     ".\n\nOpen it now?",
                    "Read", "Later")) {
      open_book(path);
    }
  }

  std::string url_;
  Settings::Catalogue auth_;
  OpdsFeed feed_;
  bool loaded_ = false;
};

// A search-format source: a server that answers queries rather than
// offering a feed to walk. One search at a time, because that is what the
// format is - there is nothing to browse between searches.
class SearchSourceBrowser : public ListView {
 public:
  SearchSourceBrowser(std::string title, const Settings::Catalogue& source)
      : ListView(std::move(title), {}, nullptr), source_(source) {
    set_empty_message("Nothing found. Try different words.");
    on_select_ = [this](int id) { activate(id); };
  }

  void on_show() override {
    if (!asked_) {
      asked_ = true;
      ask();
    }
  }

 private:
  enum { kNewSearch = -30 };

  void ask() {
    Settings::Catalogue source = source_;
    SearchSourceBrowser* self = this;
    App::instance().push(ViewPtr(new KeyboardView(
        "Search " + (source.name.empty() ? std::string("this server") : source.name), query_,
        [self](const std::string& text, bool ok) {
          App::instance().pop();
          if (!ok || trim(text).empty()) {
            // Nothing typed and nothing to show: leave rather than sit on an
            // empty list.
            if (self->results_.empty()) App::instance().pop();
            return;
          }
          self->run_search(trim(text));
        })));
  }

  void run_search(const std::string& query) {
    App& app = App::instance();
    if (!Net::instance().connected()) {
      app.show_message("Search", "Connect to Wi-Fi first: Settings has a Wi-Fi entry.");
      return;
    }
    query_ = query;
    set_title(query);
    set_status_line("Searching…");
    app.render_now();

    std::string error;
    results_.clear();
    if (!libgen_search(source_.search_address(), query, results_, error)) {
      set_status_line("");
      app.show_message("Search", error);
      rebuild();
      return;
    }
    rebuild();
    app.invalidate(Refresh::Text);
  }

  void rebuild() {
    std::vector<Item> items;
    for (size_t i = 0; i < results_.size(); ++i) {
      const SearchResult& r = results_[i];
      Item item;
      item.id = kEntryBase + (int)i;
      item.row.title = r.title.empty() ? "(untitled)" : r.title;
      std::string detail = r.author;
      if (!r.year.empty()) detail += detail.empty() ? r.year : " \xc2\xb7 " + r.year;
      item.row.subtitle = detail;
      std::string trailing = to_upper(r.extension);
      if (!r.size_text.empty()) {
        trailing += trailing.empty() ? r.size_text : " \xc2\xb7 " + r.size_text;
      }
      item.row.trailing = trailing;
      items.push_back(item);
    }
    set_actions({{"New search", kNewSearch}});
    set_status_line(query_.empty() ? "" : format("%zu result%s for \"%s\"", results_.size(),
                                                 results_.size() == 1 ? "" : "s",
                                                 query_.c_str()));
    set_items(std::move(items));
  }

  void activate(int id) {
    if (id == kNewSearch) {
      ask();
      return;
    }
    size_t index = (size_t)(id - kEntryBase);
    if (index >= results_.size()) return;
    const SearchResult& r = results_[index];
    App& app = App::instance();
    std::string details;
    if (!r.author.empty()) details += r.author + "\n";
    if (!r.year.empty()) details += r.year + "\n";
    if (!r.extension.empty() || !r.size_text.empty()) {
      details += to_upper(r.extension) + " " + r.size_text + "\n";
    }
    details += "\nSaves to " + settings().catalogue_folder + " as " + r.filename();
    if (!app.confirm(r.title, details, "Download", "Back")) return;

    set_status_line("Downloading…");
    app.render_now();
    std::string path;
    std::string error;
    if (!libgen_download(source_.search_address(), r, download_dir(), source_.user,
                         source_.password, path, error)) {
      set_status_line("");
      app.show_message("Download failed", error);
      return;
    }
    rebuild();
    if (app.confirm("Downloaded", r.title + "\n\nSaved to " + settings().catalogue_folder +
                                      ".\n\nOpen it now?",
                    "Read", "Later")) {
      open_book(path);
    }
  }

  Settings::Catalogue source_;
  std::vector<SearchResult> results_;
  std::string query_;
  bool asked_ = false;
};

// The saved list. Empty to start with, so it explains itself.
class CatalogueList : public ListView {
 public:
  CatalogueList() : ListView("Catalogues", {}, nullptr) {
    set_empty_message(
        "No catalogues yet.\n\nTap \"Add\" and enter the OPDS address of your library - "
        "Calibre-Web, Kavita, Komga, your own server, or a public catalogue. An address "
        "with {searchTerms} in it is added as a search source instead.");
    set_actions({{"Add", kAdd}, {"Find on network", kFind}});
    on_select_ = [this](int id) { activate(id); };
    on_long_press_ = [this](int id) { edit(id); };
    rebuild();
  }

  void on_show() override { rebuild(); }

 private:
  enum { kAdd = -20, kFind = -21 };

  void rebuild() {
    std::vector<Item> items;
    const std::vector<Settings::Catalogue>& list = settings().catalogues;
    for (size_t i = 0; i < list.size(); ++i) {
      Item item;
      item.id = (int)i + 1;
      item.row.title = list[i].name.empty() ? list[i].url : list[i].name;
      if (list[i].is_libgen()) {
        item.row.subtitle = "Search source \xc2\xb7 " + list[i].url;
      } else if (list[i].search_only()) {
        item.row.subtitle = "Search: " + list[i].search;
      } else {
        item.row.subtitle = list[i].url;
      }
      item.row.trailing = "›";
      items.push_back(item);
    }
    set_status_line(list.empty() ? "" : "Hold a catalogue to remove it");
    set_items(std::move(items), true);
  }

  void activate(int id) {
    if (id == kAdd) {
      ask_for_url();
      return;
    }
    if (id == kFind) {
      find_on_network();
      return;
    }
    size_t index = (size_t)(id - 1);
    const std::vector<Settings::Catalogue>& list = settings().catalogues;
    if (index >= list.size()) return;
    const Settings::Catalogue& c = list[index];
    App::instance().push(open_catalogue(c));
  }

  // Sweeps the local network for an OPDS feed. Modal on purpose: it takes a
  // few seconds and there is nothing useful to do on this screen meanwhile.
  void find_on_network() {
    App& app = App::instance();
    if (!Net::instance().connected()) {
      app.show_message("Find catalogues",
                       "Connect to Wi-Fi first: Settings has a Wi-Fi entry.");
      return;
    }
    set_status_line("Looking for catalogues on this network…");
    app.render_now();
    int last_percent = -1;
    std::vector<DiscoveredCatalogue> found = discover_catalogues(
        250, [&](int done, int total) {
          int percent = total > 0 ? done * 100 / total : 100;
          if (percent / 10 != last_percent / 10) {
            last_percent = percent;
            set_status_line(format("Looking for catalogues… %d%%", percent));
            App::instance().render_now();
          }
          return true;
        });
    set_status_line("");
    if (found.empty()) {
      app.show_message("Find catalogues",
                       "Nothing answered on this network.\n\nThe server has to be on the "
                       "same network and reachable over plain http. Add it by address "
                       "instead with \"Add\".");
      rebuild();
      return;
    }
    // Offer them one at a time: a list screen for what is usually one hit
    // costs more than it gives.
    int added = 0;
    for (const DiscoveredCatalogue& hit : found) {
      bool known = false;
      for (const Settings::Catalogue& c : settings().catalogues) {
        if (c.url == hit.url) known = true;
      }
      if (known) continue;
      if (!app.confirm("Catalogue found", hit.name + "\n\n" + hit.url + "\n\nAdd it?",
                       "Add", "Skip")) {
        continue;
      }
      Settings::Catalogue c;
      c.name = hit.name;
      c.url = hit.url;
      settings().catalogues.push_back(c);
      ++added;
    }
    if (added > 0) {
      settings().save();
      app.show_toast(format("Added %d catalogue%s", added, added == 1 ? "" : "s"));
    }
    rebuild();
    app.invalidate(Refresh::Text);
  }

  void ask_for_url() {
    App::instance().push(ViewPtr(new KeyboardView(
        "Catalogue address", "https://", [](const std::string& text, bool ok) {
          App::instance().pop();
          std::string url = trim(text);
          if (!ok || url.empty() || url == "https://") return;
          // An address with a search placeholder in it is a search
          // endpoint - which is how a server with its own search format
          // gets added exactly as it is.
          bool is_search = url.find("{searchTerms}") != std::string::npos ||
                           url.find("{searchterms}") != std::string::npos ||
                           url.find("{query}") != std::string::npos;
          // An address naming one of the Library Genesis-style endpoints is
          // that format, whether or not it carries a placeholder.
          bool is_libgen = looks_like_libgen_endpoint(url);
          // Ask for a name second: the address is the part that must be
          // right, and a name can be derived from the feed if skipped.
          App::instance().push(ViewPtr(new KeyboardView(
              "Name for this catalogue", "",
              [url, is_search, is_libgen](const std::string& name, bool named) {
                App::instance().pop();
                Settings::Catalogue c;
                if (is_libgen) {
                  c.format = "libgen";
                  c.url = url;
                } else if (is_search) {
                  c.search = url;
                } else {
                  c.url = url;
                }
                c.name = named ? trim(name) : "";
                if (c.name.empty()) {
                  Url parsed;
                  c.name = Url::parse(url, parsed) ? parsed.host : url;
                }
                settings().catalogues.push_back(c);
                settings().save();
                App::instance().show_toast("Added " + c.name);
              })));
        })));
  }

  void edit(int id) {
    size_t index = (size_t)(id - 1);
    std::vector<Settings::Catalogue>& list = settings().catalogues;
    if (index >= list.size()) return;
    std::string name = list[index].name;
    if (!App::instance().confirm("Remove catalogue", "Remove \"" + name +
                                                         "\"?\n\nBooks already downloaded "
                                                         "stay on the drive.",
                                 "Remove", "Keep")) {
      return;
    }
    list.erase(list.begin() + (long)index);
    settings().save();
    rebuild();
    App::instance().invalidate(Refresh::Text);
  }
};

}  // namespace

// Opens a saved catalogue with whichever client its format needs.
ViewPtr open_catalogue(const Settings::Catalogue& c) {
  if (c.is_libgen()) return ViewPtr(new SearchSourceBrowser(c.name, c));
  return ViewPtr(new OpdsBrowser(c.name, c.url, c));
}

ViewPtr make_catalogue_screen() { return ViewPtr(new CatalogueList()); }

ViewPtr make_catalogue_screen(const std::string& name) {
  // A named catalogue opens straight into its feed; anything else falls
  // back to the list, which explains itself when it is empty.
  for (const Settings::Catalogue& c : settings().catalogues) {
    if (icontains(c.name, name) || c.url == name) return open_catalogue(c);
  }
  CK_LOGW("opds: no saved catalogue matches \"%s\"", name.c_str());
  return make_catalogue_screen();
}

}  // namespace ck
