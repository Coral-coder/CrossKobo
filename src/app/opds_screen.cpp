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
#include "net/opds.h"
#include "platform/net.h"
#include "ui/keyboard.h"
#include "ui/list_view.h"
#include "ui/theme.h"

namespace ck {
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
    if (!loaded_) load(url_);
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

// The saved list. Empty to start with, so it explains itself.
class CatalogueList : public ListView {
 public:
  CatalogueList() : ListView("Catalogues", {}, nullptr) {
    set_empty_message(
        "No catalogues yet.\n\nTap \"Add\" and enter the OPDS address of your library - "
        "Shelfmark, Calibre-Web, Kavita, Komga and the public catalogues all work.");
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
      item.row.subtitle = list[i].url;
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
    App::instance().push(ViewPtr(new OpdsBrowser(c.name, c.url, c)));
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
          // Ask for a name second: the address is the part that must be
          // right, and a name can be derived from the feed if skipped.
          App::instance().push(ViewPtr(new KeyboardView(
              "Name for this catalogue", "", [url](const std::string& name, bool named) {
                App::instance().pop();
                Settings::Catalogue c;
                c.url = url;
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

ViewPtr make_catalogue_screen() { return ViewPtr(new CatalogueList()); }

ViewPtr make_catalogue_screen(const std::string& name) {
  // A named catalogue opens straight into its feed; anything else falls
  // back to the list, which explains itself when it is empty.
  for (const Settings::Catalogue& c : settings().catalogues) {
    if (icontains(c.name, name) || c.url == name) {
      return ViewPtr(new OpdsBrowser(c.name, c.url, c));
    }
  }
  CK_LOGW("opds: no saved catalogue matches \"%s\"", name.c_str());
  return make_catalogue_screen();
}

}  // namespace ck
