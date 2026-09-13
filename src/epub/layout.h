#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/settings.h"
#include "epub/book.h"
#include "gfx/canvas.h"
#include "gfx/font.h"

namespace ck {

// Character-level styling of a text run.
struct InlineStyle {
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strike = false;
  bool mono = false;
  bool superscript = false;
  bool subscript = false;
  float scale = 1.0f;      // multiplier on the base font size
  Color color = Color::gray(0);
  bool has_color = false;  // the book asked for this colour explicitly
  int link = -1;           // index into Document::links
};

enum class BlockKind { Paragraph, Heading, Image, Rule, ListItem, Quote, Pre, Center, Break };

// A block-level box: one paragraph, heading, list item, image and so on.
struct Block {
  BlockKind kind = BlockKind::Paragraph;
  int heading_level = 0;
  std::vector<std::pair<std::string, InlineStyle>> frags;
  std::string image_href;
  std::string marker;         // list bullet or number
  Alignment align = Alignment::Justify;
  bool align_explicit = false;
  bool centered = false;
  int indent_px = -1;         // -1 = use the reader's default
  int space_before = -1;
  int space_after = -1;
  int left_padding = 0;
  std::string id;             // anchor target
  bool page_break_before = false;
};

// One chapter, parsed and ready to lay out.
struct Document {
  std::vector<Block> blocks;
  std::vector<std::string> links;      // link index -> href
  std::map<std::string, int> anchors;  // element id -> block index
  std::string base_href;
  bool is_image_only = false;          // comic page
};

struct Run {
  int x = 0;
  int width = 0;
  std::string text;
  InlineStyle style;
  // True when a real space separates this run from the previous one. Only
  // those gaps may be stretched when justifying, otherwise punctuation
  // after </em> drifts away from the word it belongs to.
  bool space_before = false;
};

struct Line {
  int y = 0;
  int height = 0;
  int baseline = 0;
  std::vector<Run> runs;
  // Where this line starts in the document, so a page can be resumed.
  int block = 0;
  int frag = 0;
  int offset = 0;
  bool block_start = false;
  std::string image_href;   // set for image lines
  int image_width = 0;
  bool rule = false;
};

struct Page {
  int start_block = 0;
  int start_frag = 0;
  int start_offset = 0;
  std::vector<Line> lines;
  int height = 0;
};

struct LayoutParams {
  Rect content;             // where the text goes, in screen coordinates
  std::string font_family = "Literata";
  int font_px = 40;
  int line_spacing = 140;   // percent
  Alignment align = Alignment::Justify;
  bool hyphenate = true;
  bool honour_book_css = true;
  bool force_indent = false;
  bool extra_paragraph_spacing = false;
  int dpi = 300;
  bool color_images = true;
  Color text_color = Color::gray(0);
  Color link_color = Color::rgb(0x1146C8);
};

// Turns chapter markup into blocks, then blocks into pages.
class Layout {
 public:
  Layout(const Book& book, const LayoutParams& params) : book_(book), params_(params) {}

  void set_params(const LayoutParams& p) { params_ = p; }
  const LayoutParams& params() const { return params_; }

  // Parses a chapter. `spine_index` selects the section; plain-text and
  // comic sections are handled here too.
  Document parse(int spine_index) const;

  std::vector<Page> paginate(const Document& doc) const;
  // Draws one page. Returns the rectangles of any tappable links.
  void draw(Canvas& canvas, const Page& page, const Document& doc,
            std::vector<std::pair<Rect, int>>* link_rects = nullptr) const;

  // Which page contains a given document position.
  static int page_for_position(const std::vector<Page>& pages, int block, int frag, int offset);

  void clear_image_cache() { image_cache_.clear(); }

 private:
  std::vector<Line> layout_blocks(const Document& doc) const;
  const Canvas* image_for(const Document& doc, const std::string& href) const;

  const Book& book_;
  LayoutParams params_;
  mutable std::map<std::string, std::shared_ptr<Canvas>> image_cache_;
};

// Break positions inside a long word, for justified text. Conservative on
// purpose: a wrong hyphen is worse than a loose line.
std::vector<size_t> hyphenation_points(const std::string& word);

}  // namespace ck
