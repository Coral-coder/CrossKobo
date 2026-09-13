#pragma once
#include <string>
#include <vector>

#include "gfx/canvas.h"
#include "ui/view.h"

namespace ck {

struct LibraryEntry {
  std::string path;
  std::string name;      // display name (title when known)
  std::string author;
  bool is_dir = false;
  uint64_t size = 0;
  int64_t mtime = 0;
  double progress = -1.0;   // from saved state, -1 when unread
  bool finished = false;
};

// Scans a directory for readable files. Metadata comes from saved reading
// state where available, so a large library does not have to be opened and
// parsed just to be listed.
std::vector<LibraryEntry> scan_library(const std::string& dir, bool show_hidden);
bool is_readable_book(const std::string& path);

// The file browser. Starts at the user-visible storage root.
ViewPtr make_library_screen(const std::string& start_dir = "");
// Opens a book and pushes the reader; shows a message when it cannot.
void open_book(const std::string& path);

namespace covers {
// Returns a cached cover thumbnail for a book, or nullptr when it has none.
// Thumbnails are decoded once and then kept as PNG in the cache directory,
// so the home screen does not have to crack open nine EPUBs on every draw.
const Canvas* thumbnail(const std::string& book_path, int max_w, int max_h);
void clear_memory_cache();
}  // namespace covers

}  // namespace ck
