#include "library/library.h"

#include <algorithm>
#include <map>
#include <memory>

#include "app/app.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "epub/book.h"
#include "reader/reader.h"
#include "reader/state.h"
#include "ui/keyboard.h"
#include "ui/list_view.h"
#include "ui/theme.h"

namespace ck {
namespace {

constexpr int kActionSort = -2001;
constexpr int kActionSearch = -2002;
constexpr int kActionUp = -2003;

// Directories that belong to the stock firmware or to CrossKobo itself and
// only get in the way when browsing for something to read.
bool is_noise_dir(const std::string& name) {
  static const char* kNoise[] = {".kobo",    ".kobo-images", ".adds",       ".crosskobo",
                                 ".add-ons", "System Volume Information",  "$RECYCLE.BIN",
                                 ".Trashes", ".fat32-tmp",   "lost+found"};
  for (const char* n : kNoise) {
    if (name == n) return true;
  }
  return false;
}

}  // namespace

bool is_readable_book(const std::string& path) {
  std::string ext = fs::extension(path);
  return ext == "epub" || ext == "txt" || ext == "md" || ext == "cbz" || ext == "text";
}

std::vector<LibraryEntry> scan_library(const std::string& dir, bool show_hidden) {
  std::vector<LibraryEntry> out;
  for (const fs::Entry& e : fs::list_dir(dir, show_hidden)) {
    if (is_noise_dir(e.name)) continue;
    if (e.is_dir) {
      LibraryEntry entry;
      entry.path = e.path;
      entry.name = e.name;
      entry.is_dir = true;
      entry.mtime = e.mtime;
      out.push_back(std::move(entry));
      continue;
    }
    if (!is_readable_book(e.path)) continue;
    LibraryEntry entry;
    entry.path = e.path;
    entry.name = fs::stem(e.path);
    entry.size = e.size;
    entry.mtime = e.mtime;
    // Reading state also carries the real title and author, saved the first
    // time the book was opened.
    Json j;
    if (Json::parse_file(BookState::file_for(book_state_key(e.path)), j) && j.is_object()) {
      std::string title = j.get_string("title");
      if (!title.empty()) entry.name = title;
      entry.author = j.get_string("author");
      entry.progress = j.get_double("progress", -1.0);
      entry.finished = j.get_bool("finished", false);
    }
    out.push_back(std::move(entry));
  }

  const std::string& sort = settings().library_sort;
  std::sort(out.begin(), out.end(), [&](const LibraryEntry& a, const LibraryEntry& b) {
    if (a.is_dir != b.is_dir) return a.is_dir;
    if (sort == "title") return to_lower(a.name) < to_lower(b.name);
    if (sort == "author") {
      if (a.author != b.author) return to_lower(a.author) < to_lower(b.author);
      return to_lower(a.name) < to_lower(b.name);
    }
    if (sort == "added" || sort == "recent") return a.mtime > b.mtime;
    return to_lower(a.name) < to_lower(b.name);
  });
  return out;
}

void open_book(const std::string& path) {
  auto reader = std::make_unique<ReaderScreen>(path);
  if (!reader->ok()) {
    App::instance().show_message("Cannot open",
                                 fs::basename(path) +
                                     "\n\nThis file is not a readable book, or it is damaged.");
    return;
  }
  App::instance().push(std::move(reader));
  App::instance().invalidate(Refresh::Flash);
}

namespace {

class LibraryScreen : public ListView {
 public:
  explicit LibraryScreen(std::string dir) : ListView("Library", {}, nullptr), dir_(std::move(dir)) {
    if (dir_.empty()) dir_ = paths().onboard;
    on_select_ = [this](int id) { activate(id); };
    set_on_long_press([this](int id) { long_press(id); });
    reload();
  }

  void on_show() override { reload(); }

 private:
  void reload() {
    entries_ = scan_library(dir_, settings().show_hidden_files);
    std::vector<Item> items;
    int id = 0;
    for (const LibraryEntry& e : entries_) {
      Item item;
      item.id = id++;
      item.row.title = e.name;
      item.row.is_folder = e.is_dir;
      if (e.is_dir) {
        item.row.trailing = "\xE2\x80\xBA";
      } else {
        std::string detail = e.author;
        if (!detail.empty()) detail += " \xC2\xB7 ";
        detail += human_size(e.size);
        detail += " \xC2\xB7 " + std::string(fs::extension(e.path));
        item.row.subtitle = detail;
        if (e.finished) {
          item.row.trailing = "Read";
        } else if (e.progress > 0.0) {
          item.row.trailing = format("%d%%", (int)(e.progress * 100));
        }
      }
      if (!filter_.empty() && !icontains(e.name, filter_) && !icontains(e.author, filter_)) {
        continue;
      }
      items.push_back(std::move(item));
    }
    bool at_root = dir_ == paths().onboard || dir_ == "/";
    std::string title = at_root ? "Library" : fs::basename(dir_);
    if (!filter_.empty()) title += " \xC2\xB7 \"" + filter_ + "\"";
    set_title(title);
    set_items(std::move(items), true);
    set_empty_message(filter_.empty() ? "No books here.\n\nCopy EPUB, TXT or CBZ files onto the "
                                        "device over USB and they appear in this list."
                                      : "Nothing matches \"" + filter_ + "\"");
    std::vector<std::pair<std::string, int>> actions;
    if (!at_root) actions.emplace_back("Up", kActionUp);
    actions.emplace_back(sort_label(), kActionSort);
    actions.emplace_back(filter_.empty() ? "Search" : "Clear", kActionSearch);
    set_actions(std::move(actions));
    set_status_line(format("%zu item%s", entries_.size(), entries_.size() == 1 ? "" : "s"));
  }

  std::string sort_label() const {
    const std::string& s = settings().library_sort;
    if (s == "title") return "A-Z";
    if (s == "author") return "Author";
    return "Newest";
  }

  void activate(int id) {
    if (id == kActionUp) {
      dir_ = fs::dirname(dir_);
      filter_.clear();
      reload();
      App::instance().invalidate(Refresh::Flash);
      return;
    }
    if (id == kActionSort) {
      std::string& s = settings().library_sort;
      s = s == "recent" ? "title" : (s == "title" ? "author" : "recent");
      settings().save();
      reload();
      return;
    }
    if (id == kActionSearch) {
      if (!filter_.empty()) {
        filter_.clear();
        reload();
        return;
      }
      App::instance().push(std::make_unique<KeyboardView>(
          "Search the library", "", [this](const std::string& text, bool accepted) {
            if (accepted) filter_ = trim(text);
            reload();
            App::instance().invalidate(Refresh::Flash);
          }));
      return;
    }
    // Rows are indexed against the unfiltered list.
    int index = row_to_entry(id);
    if (index < 0) return;
    const LibraryEntry& e = entries_[(size_t)index];
    if (e.is_dir) {
      dir_ = e.path;
      filter_.clear();
      reload();
      App::instance().invalidate(Refresh::Flash);
      return;
    }
    open_book(e.path);
  }

  void long_press(int id) {
    int index = row_to_entry(id);
    if (index < 0) return;
    const LibraryEntry entry = entries_[(size_t)index];
    if (entry.is_dir) return;

    enum { kOpen = 1, kMarkRead, kDelete, kDetails };
    std::vector<Item> items;
    auto add = [&](int action, std::string label, std::string subtitle = "") {
      Item item;
      item.id = action;
      item.row.title = std::move(label);
      item.row.subtitle = std::move(subtitle);
      items.push_back(std::move(item));
    };
    add(kOpen, "Open");
    add(kMarkRead, entry.finished ? "Mark as unread" : "Mark as finished");
    add(kDetails, "Details", entry.path);
    add(kDelete, "Delete from device");

    App::instance().push(std::make_unique<ListView>(entry.name, std::move(items),
                                                    [this, entry](int action) {
      switch (action) {
        case kOpen:
          App::instance().pop();
          open_book(entry.path);
          break;
        case kMarkRead: {
          // Flip the flag in the saved state without opening the book.
          std::string key = book_state_key(entry.path);
          Json j;
          Json::parse_file(BookState::file_for(key), j);
          if (!j.is_object()) j = Json::object();
          j["key"] = Json(key);
          j["path"] = Json(entry.path);
          if (j.get_string("title").empty()) j["title"] = Json(entry.name);
          j["finished"] = Json(!entry.finished);
          j.save_file(BookState::file_for(key), false);
          App::instance().pop();
          reload();
          App::instance().invalidate(Refresh::Flash);
          break;
        }
        case kDetails:
          App::instance().show_message(
              entry.name,
              format("%s\n\n%s\nModified %s\n\n%s",
                     entry.author.empty() ? "Unknown author" : entry.author.c_str(),
                     human_size(entry.size).c_str(), relative_time(entry.mtime).c_str(),
                     entry.path.c_str()));
          break;
        case kDelete:
          if (App::instance().confirm("Delete book",
                                      "Permanently delete\n" + fs::basename(entry.path) + "?",
                                      "Delete", "Keep")) {
            fs::remove_file(entry.path);
            Recents::instance().remove(entry.path);
            App::instance().pop();
            reload();
            App::instance().invalidate(Refresh::Flash);
          }
          break;
        default:
          break;
      }
    }));
  }

  int row_to_entry(int id) const {
    return id >= 0 && id < (int)entries_.size() ? id : -1;
  }

  std::string dir_;
  std::string filter_;
  std::vector<LibraryEntry> entries_;
};

}  // namespace

ViewPtr make_library_screen(const std::string& start_dir) {
  return std::make_unique<LibraryScreen>(start_dir);
}

namespace covers {
namespace {

std::map<std::string, std::shared_ptr<Canvas>> g_cache;

std::string cache_path(const std::string& book_path, int w, int h) {
  return format("%s/covers/%s_%dx%d.png", paths().cache_dir().c_str(),
                book_state_key(book_path).c_str(), w, h);
}

}  // namespace

const Canvas* thumbnail(const std::string& book_path, int max_w, int max_h) {
  std::string key = cache_path(book_path, max_w, max_h);
  auto it = g_cache.find(key);
  if (it != g_cache.end()) return it->second.get();

  auto canvas = std::make_shared<Canvas>();
  if (fs::exists(key) && Canvas::load_image(key, *canvas)) {
    g_cache[key] = canvas;
    return canvas.get();
  }

  Book book;
  if (!book.open(book_path)) {
    g_cache[key] = nullptr;
    return nullptr;
  }
  const Canvas* full = book.cover();
  if (!full || !full->valid()) {
    g_cache[key] = nullptr;
    return nullptr;
  }
  double scale = std::min((double)max_w / full->width(), (double)max_h / full->height());
  int w = std::max(1, (int)(full->width() * scale));
  int h = std::max(1, (int)(full->height() * scale));
  canvas->reset(w, h);
  canvas->clear(Color::gray(255));
  canvas->blit_scaled(*full, Rect(0, 0, w, h));
  canvas->save_png(key);
  g_cache[key] = canvas;
  return canvas.get();
}

void clear_memory_cache() { g_cache.clear(); }

}  // namespace covers

}  // namespace ck
