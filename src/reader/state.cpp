#include "reader/state.h"

#include <algorithm>

#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"

namespace ck {
namespace {

constexpr size_t kMaxRecents = 24;
constexpr size_t kMaxDailyDays = 30;

std::string today_key() {
  time_t t = (time_t)wall_seconds();
  struct tm tm_buf;
  localtime_r(&t, &tm_buf);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
  return buf;
}

}  // namespace

std::string BookState::file_for(const std::string& key) {
  return paths().data + "/books/" + key + ".json";
}

BookState BookState::load(const Book& book) {
  BookState st;
  st.key = book.cache_key();
  st.path = book.path();
  st.title = book.metadata().title;
  st.author = book.metadata().author;

  Json j;
  if (Json::parse_file(file_for(st.key), j) && j.is_object()) {
    st.spine = j.get_int("spine", 0);
    st.block = j.get_int("block", 0);
    st.frag = j.get_int("frag", 0);
    st.offset = j.get_int("offset", 0);
    st.progress = j.get_double("progress", 0.0);
    st.finished = j.get_bool("finished", false);
    st.first_opened = j.get_int64("firstOpened", 0);
    st.last_read = j.get_int64("lastRead", 0);
    st.total_seconds = j.get_int64("totalSeconds", 0);
    st.sessions = j.get_int("sessions", 0);
    st.pages_turned = j.get_int("pagesTurned", 0);
    if (const Json* marks = j.find("bookmarks")) {
      for (const Json& m : marks->items()) {
        Bookmark b;
        b.spine = m.get_int("spine", 0);
        b.block = m.get_int("block", 0);
        b.frag = m.get_int("frag", 0);
        b.offset = m.get_int("offset", 0);
        b.snippet = m.get_string("snippet");
        b.created = m.get_int64("created", 0);
        st.bookmarks.push_back(std::move(b));
      }
    }
  }
  if (st.first_opened == 0) st.first_opened = wall_seconds();
  return st;
}

void BookState::save() const {
  Json j = Json::object();
  j["key"] = Json(key);
  j["path"] = Json(path);
  j["title"] = Json(title);
  j["author"] = Json(author);
  j["spine"] = Json(spine);
  j["block"] = Json(block);
  j["frag"] = Json(frag);
  j["offset"] = Json(offset);
  j["progress"] = Json(progress);
  j["finished"] = Json(finished);
  j["firstOpened"] = Json(first_opened);
  j["lastRead"] = Json(last_read);
  j["totalSeconds"] = Json(total_seconds);
  j["sessions"] = Json(sessions);
  j["pagesTurned"] = Json(pages_turned);
  Json marks = Json::array();
  for (const Bookmark& b : bookmarks) {
    Json m = Json::object();
    m["spine"] = Json(b.spine);
    m["block"] = Json(b.block);
    m["frag"] = Json(b.frag);
    m["offset"] = Json(b.offset);
    m["snippet"] = Json(b.snippet);
    m["created"] = Json(b.created);
    marks.push_back(std::move(m));
  }
  j["bookmarks"] = std::move(marks);
  j.save_file(file_for(key), false);
}

bool BookState::has_bookmark_at(int spine_index, int block_index) const {
  for (const Bookmark& b : bookmarks) {
    if (b.spine == spine_index && b.block == block_index) return true;
  }
  return false;
}

void BookState::toggle_bookmark(const Bookmark& mark) {
  for (size_t i = 0; i < bookmarks.size(); ++i) {
    if (bookmarks[i].spine == mark.spine && bookmarks[i].block == mark.block) {
      bookmarks.erase(bookmarks.begin() + (long)i);
      return;
    }
  }
  bookmarks.push_back(mark);
  std::sort(bookmarks.begin(), bookmarks.end(), [](const Bookmark& a, const Bookmark& b) {
    if (a.spine != b.spine) return a.spine < b.spine;
    return a.block < b.block;
  });
}

// ----------------------------------------------------------------- Stats

Stats& Stats::instance() {
  static Stats s;
  return s;
}

void Stats::load() {
  Json j;
  if (!Json::parse_file(paths().stats_file(), j) || !j.is_object()) {
    first_use = wall_seconds();
    return;
  }
  total_seconds = j.get_int64("totalSeconds", 0);
  sessions = j.get_int("sessions", 0);
  pages_turned = j.get_int("pagesTurned", 0);
  books_finished = j.get_int("booksFinished", 0);
  first_use = j.get_int64("firstUse", wall_seconds());
  longest_session = j.get_int64("longestSession", 0);
  daily.clear();
  if (const Json* d = j.find("daily")) {
    for (const auto& kv : d->members()) daily.emplace_back(kv.first, kv.second.as_int64(0));
  }
}

void Stats::save() const {
  Json j = Json::object();
  j["totalSeconds"] = Json(total_seconds);
  j["sessions"] = Json(sessions);
  j["pagesTurned"] = Json(pages_turned);
  j["booksFinished"] = Json(books_finished);
  j["firstUse"] = Json(first_use);
  j["longestSession"] = Json(longest_session);
  Json d = Json::object();
  for (const auto& kv : daily) d[kv.first] = Json(kv.second);
  j["daily"] = std::move(d);
  j.save_file(paths().stats_file(), true);
}

void Stats::add_reading(int64_t seconds, int pages, bool new_session) {
  if (seconds < 5 && pages == 0) return;  // ignore accidental opens
  total_seconds += seconds;
  pages_turned += pages;
  if (new_session) ++sessions;
  longest_session = std::max(longest_session, seconds);
  if (first_use == 0) first_use = wall_seconds();

  std::string key = today_key();
  for (auto& kv : daily) {
    if (kv.first == key) {
      kv.second += seconds;
      save();
      return;
    }
  }
  daily.emplace_back(key, seconds);
  if (daily.size() > kMaxDailyDays) daily.erase(daily.begin());
  save();
}

void Stats::note_finished() {
  ++books_finished;
  save();
}

int64_t Stats::seconds_today() const {
  std::string key = today_key();
  for (const auto& kv : daily) {
    if (kv.first == key) return kv.second;
  }
  return 0;
}

int Stats::streak_days() const {
  // Walk back day by day while there is recorded reading time.
  int streak = 0;
  time_t t = (time_t)wall_seconds();
  for (int i = 0; i < (int)kMaxDailyDays; ++i) {
    struct tm tm_buf;
    time_t day = t - (time_t)i * 86400;
    localtime_r(&day, &tm_buf);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    bool found = false;
    for (const auto& kv : daily) {
      if (kv.first == buf && kv.second > 0) {
        found = true;
        break;
      }
    }
    if (!found) break;
    ++streak;
  }
  return streak;
}

// ---------------------------------------------------------------- Recents

Recents& Recents::instance() {
  static Recents r;
  return r;
}

void Recents::load() {
  entries_.clear();
  Json j;
  if (!Json::parse_file(paths().recents_file(), j) || !j.is_array()) return;
  for (const Json& item : j.items()) {
    RecentEntry e;
    e.path = item.get_string("path");
    if (e.path.empty()) continue;
    e.title = item.get_string("title");
    e.author = item.get_string("author");
    e.progress = item.get_double("progress", 0.0);
    e.last_read = item.get_int64("lastRead", 0);
    e.finished = item.get_bool("finished", false);
    entries_.push_back(std::move(e));
  }
}

void Recents::save() const {
  Json j = Json::array();
  for (const RecentEntry& e : entries_) {
    Json item = Json::object();
    item["path"] = Json(e.path);
    item["title"] = Json(e.title);
    item["author"] = Json(e.author);
    item["progress"] = Json(e.progress);
    item["lastRead"] = Json(e.last_read);
    item["finished"] = Json(e.finished);
    j.push_back(std::move(item));
  }
  j.save_file(paths().recents_file(), false);
}

void Recents::touch(const BookState& state) {
  remove(state.path);
  if (state.finished && settings().remove_read_books_from_recents) {
    save();
    return;
  }
  RecentEntry e;
  e.path = state.path;
  e.title = state.title;
  e.author = state.author;
  e.progress = state.progress;
  e.last_read = state.last_read ? state.last_read : wall_seconds();
  e.finished = state.finished;
  entries_.insert(entries_.begin(), std::move(e));
  if (entries_.size() > kMaxRecents) entries_.resize(kMaxRecents);
  save();
}

void Recents::remove(const std::string& path) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const RecentEntry& e) { return e.path == path; }),
                 entries_.end());
}

void Recents::prune() {
  size_t before = entries_.size();
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [](const RecentEntry& e) { return !fs::exists(e.path); }),
                 entries_.end());
  if (entries_.size() != before) save();
}

}  // namespace ck
