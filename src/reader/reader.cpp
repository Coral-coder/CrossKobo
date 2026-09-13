#include "reader/reader.h"

#include <algorithm>
#include <cmath>

#include "app/app.h"
#include "app/settings.h"
#include "core/clock.h"
#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "notes/notes.h"
#include "platform/power.h"
#include "platform/screen.h"
#include "ui/icons.h"
#include "ui/list_view.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace ck {
namespace {

constexpr int64_t kSaveIntervalMs = 30000;

}  // namespace

ReaderScreen::ReaderScreen(std::string path) {
  ok_ = book_.open(path);
  if (!ok_) {
    CK_LOGW("reader: cannot open %s", path.c_str());
    return;
  }
  state_ = BookState::load(book_);
  layout_ = std::make_unique<Layout>(book_, make_params());
  compute_weights();
  spine_index_ = std::max(0, std::min((int)book_.spine().size() - 1, state_.spine));
  load_chapter(spine_index_);
  page_index_ = Layout::page_for_position(pages_, state_.block, state_.frag, state_.offset);
}

ReaderScreen::~ReaderScreen() = default;

Rect ReaderScreen::content_rect() const {
  Rect bounds = Screen::instance().bounds();
  const Settings& s = settings();
  int margin = s.screen_margin;
  int top = margin;
  int bottom = margin;
  // The status bar lives in the bottom margin unless focus mode hides it.
  bool status = !s.focus_reading &&
                (s.status_bar_title || s.status_bar_clock || s.status_bar_battery ||
                 s.status_bar_progress_bar || s.status_bar_chapter_pages ||
                 s.status_bar_percentage);
  if (status) bottom += theme().small_px + 12;
  return Rect(bounds.x + margin, bounds.y + top, bounds.w - 2 * margin,
              bounds.h - top - bottom);
}

LayoutParams ReaderScreen::make_params() const {
  const Settings& s = settings();
  LayoutParams p;
  p.content = content_rect();
  p.font_family = s.font_family;
  p.font_px = s.font_px(Screen::instance().dpi());
  p.line_spacing = s.line_spacing;
  p.align = s.alignment;
  p.hyphenate = s.hyphenation;
  p.honour_book_css = s.embedded_style;
  p.force_indent = s.force_paragraph_indent;
  p.extra_paragraph_spacing = s.extra_paragraph_spacing;
  p.dpi = Screen::instance().dpi();
  p.color_images = s.image_rendering != ImageRendering::Grayscale;
  p.text_color = theme().fg;
  p.link_color = theme().accent;
  return p;
}

void ReaderScreen::compute_weights() {
  // Relative chapter sizes give an overall progress percentage without
  // paginating the whole book up front.
  chapter_weights_.clear();
  total_weight_ = 0.0;
  for (size_t i = 0; i < book_.spine().size(); ++i) {
    // The uncompressed size of the chapter file is a good proxy for how
    // much reading it holds, and it costs nothing to obtain. Each chapter's
    // weight is replaced by its real character count once it is parsed.
    double weight = (double)std::max<uint64_t>(256, book_.spine_size((int)i));
    chapter_weights_.push_back(weight);
    total_weight_ += weight;
  }
}

void ReaderScreen::load_chapter(int spine_index, bool at_end) {
  spine_index_ = std::max(0, std::min((int)book_.spine().size() - 1, spine_index));
  layout_->set_params(make_params());
  doc_ = layout_->parse(spine_index_);
  pages_ = layout_->paginate(doc_);
  if (pages_.empty()) pages_.push_back(Page());
  page_index_ = at_end ? (int)pages_.size() - 1 : 0;

  // Refine this chapter's weight now that we know how much text it holds.
  double chars = 0;
  for (const Block& b : doc_.blocks) {
    for (const auto& f : b.frags) chars += (double)f.first.size();
  }
  if (spine_index_ < (int)chapter_weights_.size() && chars > 0) {
    total_weight_ += (chars - chapter_weights_[spine_index_]);
    chapter_weights_[spine_index_] = chars;
  }
  full_refresh_next_ = true;
}

void ReaderScreen::update_progress() {
  double before = 0.0;
  for (int i = 0; i < spine_index_ && i < (int)chapter_weights_.size(); ++i) {
    before += chapter_weights_[i];
  }
  double current = spine_index_ < (int)chapter_weights_.size() ? chapter_weights_[spine_index_]
                                                              : 1.0;
  double within = pages_.size() > 1 ? (double)page_index_ / (double)(pages_.size() - 1) : 1.0;
  double total = std::max(1.0, total_weight_);
  state_.progress = std::max(0.0, std::min(1.0, (before + current * within) / total));

  if (!pages_.empty()) {
    const Page& page = pages_[page_index_];
    state_.spine = spine_index_;
    state_.block = page.start_block;
    state_.frag = page.start_frag;
    state_.offset = page.start_offset;
  }
  state_.last_read = wall_seconds();
}

void ReaderScreen::on_show() {
  session_start_ms_ = now_ms();
  session_pages_ = 0;
  last_auto_turn_ms_ = now_ms();
  last_save_ms_ = now_ms();
  full_refresh_next_ = true;
}

void ReaderScreen::on_hide() {
  // on_hide also fires when a menu opens on top, so only the reading time
  // and pages accumulate here; the session itself is counted once.
  int64_t seconds = (now_ms() - session_start_ms_) / 1000;
  if (seconds > 0 || session_pages_ > 0) {
    state_.total_seconds += seconds;
    state_.pages_turned += session_pages_;
    bool first = !session_counted_;
    if (first) {
      ++state_.sessions;
      session_counted_ = true;
    }
    Stats::instance().add_reading(seconds, session_pages_, first);
  }
  session_start_ms_ = now_ms();
  session_pages_ = 0;
  update_progress();
  save_now();
}

void ReaderScreen::save_now() {
  state_.save();
  Recents::instance().touch(state_);
  last_save_ms_ = now_ms();
}

void ReaderScreen::next_page() {
  if (page_index_ + 1 < (int)pages_.size()) {
    ++page_index_;
  } else if (spine_index_ + 1 < (int)book_.spine().size()) {
    load_chapter(spine_index_ + 1);
  } else {
    // End of the book.
    if (!state_.finished) {
      App::instance().show_toast("End of book");
      mark_finished(true);
    }
    return;
  }
  ++session_pages_;
  Screen::instance().note_page_turn();
  update_progress();
  if (now_ms() - last_save_ms_ > kSaveIntervalMs) save_now();
  last_auto_turn_ms_ = now_ms();
}

void ReaderScreen::previous_page() {
  if (page_index_ > 0) {
    --page_index_;
  } else if (spine_index_ > 0) {
    load_chapter(spine_index_ - 1, true);
  } else {
    return;
  }
  ++session_pages_;
  Screen::instance().note_page_turn();
  update_progress();
  last_auto_turn_ms_ = now_ms();
}

void ReaderScreen::go_to_chapter(int spine_index, const std::string& fragment) {
  load_chapter(spine_index);
  if (!fragment.empty()) {
    auto it = doc_.anchors.find(fragment);
    if (it != doc_.anchors.end()) {
      page_index_ = Layout::page_for_position(pages_, it->second, 0, 0);
    }
  }
  update_progress();
  save_now();
}

void ReaderScreen::go_to_percent(double fraction) {
  fraction = std::max(0.0, std::min(1.0, fraction));
  double target = fraction * std::max(1.0, total_weight_);
  double accumulated = 0.0;
  for (size_t i = 0; i < chapter_weights_.size(); ++i) {
    if (accumulated + chapter_weights_[i] >= target || i + 1 == chapter_weights_.size()) {
      load_chapter((int)i);
      double within = chapter_weights_[i] > 0
                          ? (target - accumulated) / chapter_weights_[i]
                          : 0.0;
      page_index_ = std::max(
          0, std::min((int)pages_.size() - 1,
                      (int)std::lround(within * (double)std::max<size_t>(1, pages_.size() - 1))));
      break;
    }
    accumulated += chapter_weights_[i];
  }
  update_progress();
  save_now();
}

void ReaderScreen::go_to_position(int spine, int block, int frag, int offset) {
  load_chapter(spine);
  page_index_ = Layout::page_for_position(pages_, block, frag, offset);
  update_progress();
}

void ReaderScreen::relayout() {
  // Keep the reader on the same words across a typography change.
  int block = 0, frag = 0, offset = 0;
  if (!pages_.empty()) {
    block = pages_[page_index_].start_block;
    frag = pages_[page_index_].start_frag;
    offset = pages_[page_index_].start_offset;
  }
  layout_->set_params(make_params());
  layout_->clear_image_cache();
  pages_ = layout_->paginate(doc_);
  if (pages_.empty()) pages_.push_back(Page());
  page_index_ = Layout::page_for_position(pages_, block, frag, offset);
  update_progress();
  full_refresh_next_ = true;
}

std::string ReaderScreen::current_page_text() const {
  if (pages_.empty()) return "";
  std::string out;
  for (const Line& line : pages_[page_index_].lines) {
    for (const Run& run : line.runs) {
      if (!out.empty() && !run.text.empty()) out += ' ';
      out += run.text;
    }
  }
  return out;
}

void ReaderScreen::mark_finished(bool finished) {
  state_.finished = finished;
  if (finished) {
    Stats::instance().note_finished();
    if (settings().move_finished_to_read_folder) {
      std::string dir = fs::dirname(state_.path);
      std::string target = fs::join_path(fs::join_path(dir, "Read"), fs::basename(state_.path));
      if (fs::rename(state_.path, target)) {
        CK_LOGI("reader: moved finished book to %s", target.c_str());
        Recents::instance().remove(state_.path);
        state_.path = target;
      }
    }
  }
  save_now();
}

void ReaderScreen::toggle_bookmark() {
  if (pages_.empty()) return;
  const Page& page = pages_[page_index_];
  Bookmark mark;
  mark.spine = spine_index_;
  mark.block = page.start_block;
  mark.frag = page.start_frag;
  mark.offset = page.start_offset;
  mark.created = wall_seconds();
  std::string text = current_page_text();
  mark.snippet = text.size() > 90 ? text.substr(0, 90) + "\xE2\x80\xA6" : text;
  bool had = state_.has_bookmark_at(mark.spine, mark.block);
  state_.toggle_bookmark(mark);
  save_now();
  App::instance().show_toast(had ? "Bookmark removed" : "Bookmark added");
}

// -------------------------------------------------------------------- draw

void ReaderScreen::draw_status_bar(Canvas& canvas, const Rect& bounds) {
  const Settings& s = settings();
  if (s.focus_reading) return;
  const Theme& th = theme();
  Rect content = content_rect();
  int y = content.bottom() + 4;
  int h = th.small_px + 8;
  Rect bar(content.x, y, content.w, h);

  TextStyle st = ui_style(th.small_px, th.muted);
  int right = bar.right();

  if (s.status_bar_battery) {
    BatteryState battery = Power::instance().battery();
    if (battery.percent >= 0) {
      int icon_w = th.small_px + 8;
      Rect icon(right - icon_w, bar.y + 2, icon_w, th.small_px - 2);
      draw_battery_icon(canvas, icon, battery.percent, battery.charging);
      right = icon.x - 8;
      if (!s.hide_battery_percentage) {
        std::string pct = format("%d%%", battery.percent);
        int w = text_width(pct, st);
        draw_text_in(canvas, Rect(right - w, bar.y, w, h), pct, st, 1);
        right -= w + 10;
      }
    }
  }
  if (s.status_bar_clock) {
    std::string clock = format_time(wall_seconds(), s.clock_24h);
    int w = text_width(clock, st);
    draw_text_in(canvas, Rect(right - w, bar.y, w, h), clock, st, 1);
    right -= w + 14;
  }
  if (s.status_bar_percentage) {
    std::string pct = format("%d%%", (int)std::lround(state_.progress * 100.0));
    int w = text_width(pct, st);
    draw_text_in(canvas, Rect(right - w, bar.y, w, h), pct, st, 1);
    right -= w + 14;
  }

  int left = bar.x;
  if (s.status_bar_chapter_pages && !pages_.empty()) {
    std::string pages = format("%d / %d", page_index_ + 1, (int)pages_.size());
    int w = text_width(pages, st);
    draw_text_in(canvas, Rect(left, bar.y, w, h), pages, st, -1);
    left += w + 14;
  }
  if (s.status_bar_title) {
    // A star at the left of the status bar marks a bookmarked page.
    if (!pages_.empty() &&
        state_.has_bookmark_at(spine_index_, pages_[page_index_].start_block)) {
      Rect star(left, bar.y + 2, th.small_px, th.small_px);
      draw_icon(canvas, star, Icon::Star, th.fg, 2);
      left += th.small_px + 6;
    }
    std::string title = book_.metadata().title;
    draw_text_in(canvas, Rect(left, bar.y, std::max(0, right - left - 10), h),
                 ellipsize(title, st, std::max(0, right - left - 10)), st, -1);
  }
  if (s.status_bar_progress_bar) {
    // Pinned to the bottom edge of the screen, so a thick bar cannot spill
    // past the margin and get clipped.
    int thickness = s.status_bar_progress_thickness;
    Rect track(content.x, bounds.bottom() - thickness - 2, content.w, thickness);
    draw_progress_bar(canvas, track, state_.progress, thickness);
  }
}

void ReaderScreen::draw(Canvas& canvas, const Rect& bounds) {
  const Theme& th = theme();
  canvas.clear(th.bg);
  if (!ok_) {
    draw_centered_message(canvas, bounds, "This book could not be opened.");
    return;
  }
  link_rects_.clear();
  if (!pages_.empty()) {
    layout_->draw(canvas, pages_[page_index_], doc_, &link_rects_);
    if (settings().guide_dots) {
      // A dotted rule under each line helps some readers track position.
      Rect content = content_rect();
      for (const Line& line : pages_[page_index_].lines) {
        if (line.runs.empty()) continue;
        canvas.draw_dotted_hline(content.x, content.y + line.y + line.height - 3, content.w,
                                 th.faint, 2, 8);
      }
    }
  }
  draw_status_bar(canvas, bounds);
}

Refresh ReaderScreen::refresh_hint() const {
  if (full_refresh_next_) return Refresh::Image;
  bool colour_page = false;
  if (!pages_.empty()) {
    for (const Line& line : pages_[page_index_].lines) {
      if (!line.image_href.empty()) {
        colour_page = true;
        break;
      }
      for (const Run& run : line.runs) {
        if (run.style.has_color) {
          colour_page = true;
          break;
        }
      }
      if (colour_page) break;
    }
  }
  if (colour_page && settings().image_rendering != ImageRendering::Grayscale) {
    return Refresh::Color;
  }
  return Refresh::Text;
}

int ReaderScreen::tick_ms() const {
  if (settings().auto_page_turn_seconds > 0) return 1000;
  return settings().status_bar_clock ? 30000 : -1;
}

bool ReaderScreen::on_tick() {
  int seconds = settings().auto_page_turn_seconds;
  if (seconds > 0 && now_ms() - last_auto_turn_ms_ >= (int64_t)seconds * 1000) {
    next_page();
    return true;
  }
  return settings().status_bar_clock;  // repaint for the clock
}

// ------------------------------------------------------------------- input

bool ReaderScreen::follow_link(int link_index) {
  if (link_index < 0 || link_index >= (int)doc_.links.size()) return false;
  const std::string& href = doc_.links[link_index];
  std::string fragment = book_.fragment_of(href);
  std::string target = book_.resolve_href(href, doc_.base_href);
  if (target.empty() && !fragment.empty()) {
    // Same-document jump.
    auto it = doc_.anchors.find(fragment);
    if (it != doc_.anchors.end()) {
      show_footnote(href);
      return true;
    }
    return false;
  }
  show_footnote(href);
  return true;
}

void ReaderScreen::show_footnote(const std::string& href) {
  // Short targets are shown as a popup (the usual footnote case); anything
  // longer is a real navigation.
  std::string fragment = book_.fragment_of(href);
  int target_spine = book_.spine_index_for_href(href);
  if (target_spine < 0) target_spine = spine_index_;

  Document target_doc =
      target_spine == spine_index_ ? doc_ : layout_->parse(target_spine);
  int block = 0;
  if (!fragment.empty()) {
    auto it = target_doc.anchors.find(fragment);
    if (it == target_doc.anchors.end()) {
      go_to_chapter(target_spine, fragment);
      return;
    }
    block = it->second;
  }

  std::string text;
  for (int i = block; i < (int)target_doc.blocks.size() && i < block + 3; ++i) {
    for (const auto& frag : target_doc.blocks[i].frags) text += frag.first;
    text += "\n";
    if (text.size() > 600) break;
  }
  text = trim(text);
  if (text.empty() || text.size() > 700) {
    go_to_chapter(target_spine, fragment);
    return;
  }
  App::instance().show_message("Note", text);
}

bool ReaderScreen::handle(const InputEvent& event) {
  const Settings& s = settings();
  Rect bounds = Screen::instance().bounds();
  full_refresh_next_ = false;

  switch (event.type) {
    case EventType::KeyDown: {
      bool forward = event.key == Key::PageForward;
      bool back = event.key == Key::PageBack;
      if (s.buttons_swapped) std::swap(forward, back);
      // Held upside down, the top button is now the bottom one.
      if (s.buttons_follow_rotation && (s.rotation == 2 || s.rotation == 3)) {
        std::swap(forward, back);
      }
      if (forward) {
        next_page();
        return true;
      }
      if (back) {
        previous_page();
        return true;
      }
      if (event.key == Key::Home) {
        App::instance().pop_to_root();
        return true;
      }
      if (event.key == Key::Menu) {
        open_menu();
        return true;
      }
      return false;
    }
    case EventType::Tap: {
      // Links first: a footnote marker is small, so it wins over zones.
      for (const auto& link : link_rects_) {
        if (link.first.inset(-6).contains(event.x, event.y) && follow_link(link.second)) {
          return true;
        }
      }
      if (s.tap_zones == TapZones::Off) return false;
      int third = bounds.w / 3;
      bool left = event.x < third;
      bool right = event.x > bounds.w - third;
      if (s.tap_zones == TapZones::Inverted) std::swap(left, right);
      // The middle band opens the menu when enabled.
      if (!left && !right) {
        if (s.tap_for_reader_menu) {
          open_menu();
          return true;
        }
        next_page();
        return true;
      }
      if (right) {
        next_page();
      } else {
        previous_page();
      }
      return true;
    }
    case EventType::LongPress:
      toggle_bookmark();
      return true;
    case EventType::Swipe:
      switch (event.swipe) {
        case SwipeDir::Left:
          next_page();
          return true;
        case SwipeDir::Right:
          previous_page();
          return true;
        case SwipeDir::Up:
          open_menu();
          return true;
        case SwipeDir::Down:
          open_toc();
          return true;
        default:
          return false;
      }
    default:
      return false;
  }
}

}  // namespace ck
