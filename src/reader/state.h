#pragma once
#include <string>
#include <vector>

#include "core/json.h"
#include "epub/book.h"

namespace ck {

// Identity of a book for state and cache purposes: stable across a re-copy
// of the same file, and distinct between two books that share a title.
std::string book_state_key(const std::string& path);

struct Bookmark {
  int spine = 0;
  int block = 0;
  int frag = 0;
  int offset = 0;
  std::string snippet;
  int64_t created = 0;
};

// Everything CrossKobo remembers about one book. Stored next to the
// library on the user-visible partition so it survives a reinstall.
struct BookState {
  std::string key;
  std::string path;
  std::string title;
  std::string author;

  int spine = 0;
  int block = 0;
  int frag = 0;
  int offset = 0;
  double progress = 0.0;      // 0..1 through the whole book
  bool finished = false;

  int64_t first_opened = 0;
  int64_t last_read = 0;
  int64_t total_seconds = 0;
  int sessions = 0;
  int pages_turned = 0;

  std::vector<Bookmark> bookmarks;

  static std::string file_for(const std::string& key);
  static BookState load(const Book& book);
  void save() const;
  bool has_bookmark_at(int spine, int block) const;
  void toggle_bookmark(const Bookmark& mark);
};

// Reading statistics across the whole library, mirroring the set CrossInk
// tracks: books read, total time, sessions, pages turned, average session
// length and pages per minute.
struct Stats {
  int64_t total_seconds = 0;
  int sessions = 0;
  int pages_turned = 0;
  int books_finished = 0;
  int64_t first_use = 0;
  int64_t longest_session = 0;
  // Seconds read per day, keyed by date, kept for the last 30 days so the
  // dashboard can draw a streak without unbounded growth.
  std::vector<std::pair<std::string, int64_t>> daily;

  static Stats& instance();
  void load();
  void save() const;
  // `new_session` distinguishes opening a book from merely coming back
  // from a menu, which would otherwise inflate the session count.
  void add_reading(int64_t seconds, int pages, bool new_session);
  void note_finished();
  int64_t average_session() const { return sessions ? total_seconds / sessions : 0; }
  double pages_per_minute() const {
    return total_seconds > 0 ? (double)pages_turned * 60.0 / (double)total_seconds : 0.0;
  }
  int64_t seconds_today() const;
  int streak_days() const;
};

// The recently-read list shown on the home screen.
struct RecentEntry {
  std::string path;
  std::string title;
  std::string author;
  double progress = 0.0;
  int64_t last_read = 0;
  bool finished = false;
};

class Recents {
 public:
  static Recents& instance();
  void load();
  void save() const;
  void touch(const BookState& state);
  void remove(const std::string& path);
  const std::vector<RecentEntry>& entries() const { return entries_; }
  // Drops entries whose file no longer exists.
  void prune();

 private:
  std::vector<RecentEntry> entries_;
};

}  // namespace ck
