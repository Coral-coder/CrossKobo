#pragma once
#include <memory>
#include <string>
#include <vector>

#include "epub/book.h"
#include "epub/layout.h"
#include "reader/state.h"
#include "ui/view.h"

namespace ck {

// The reading view. Owns the open book, the paginated current chapter and
// the reading session accounting.
class ReaderScreen : public View {
 public:
  explicit ReaderScreen(std::string path);
  ~ReaderScreen() override;

  bool ok() const { return ok_; }
  const Book& book() const { return book_; }
  BookState& state() { return state_; }

  void on_show() override;
  void on_hide() override;
  void draw(Canvas& canvas, const Rect& bounds) override;
  bool handle(const InputEvent& event) override;
  Refresh refresh_hint() const override;
  int tick_ms() const override;
  bool on_tick() override;
  std::string title() const override { return book_.metadata().title; }

  // --------------------------------------------------------- navigation
  void next_page();
  void previous_page();
  void go_to_chapter(int spine_index, const std::string& fragment = "");
  void go_to_percent(double fraction);
  void go_to_position(int spine, int block, int frag, int offset);
  double progress() const { return state_.progress; }
  int chapter_page() const { return page_index_ + 1; }
  int chapter_pages() const { return (int)pages_.size(); }

  // ------------------------------------------------------------ actions
  void open_menu();
  void open_toc();
  void open_bookmarks();
  void toggle_bookmark();
  void mark_finished(bool finished);
  void relayout();           // after a typography change
  std::string current_page_text() const;

 private:
  void load_chapter(int spine_index, bool at_end = false);
  void update_progress();
  void compute_weights();
  Rect content_rect() const;
  LayoutParams make_params() const;
  void draw_status_bar(Canvas& canvas, const Rect& bounds);
  bool follow_link(int link_index);
  void show_footnote(const std::string& href);
  void save_now();

  Book book_;
  BookState state_;
  std::unique_ptr<Layout> layout_;
  Document doc_;
  std::vector<Page> pages_;
  std::vector<std::pair<Rect, int>> link_rects_;
  std::vector<double> chapter_weights_;  // relative size of each spine item
  double total_weight_ = 0.0;

  int spine_index_ = 0;
  int page_index_ = 0;
  bool ok_ = false;
  bool full_refresh_next_ = true;

  int64_t session_start_ms_ = 0;
  int session_pages_ = 0;
  int64_t last_auto_turn_ms_ = 0;
  int64_t last_save_ms_ = 0;
};

}  // namespace ck
