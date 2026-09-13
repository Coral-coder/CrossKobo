#include "epub/layout.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/fs.h"
#include "core/log.h"
#include "core/str.h"
#include "epub/xml.h"

namespace ck {
namespace {

bool is_block_tag(const std::string& tag) {
  static const char* kBlocks[] = {
      "p",   "div", "h1", "h2",     "h3",         "h4",   "h5",  "h6", "li",
      "ul",  "ol",  "blockquote",   "pre",        "table", "tr", "td", "th",
      "section", "article", "header", "footer", "aside", "nav", "figure",
      "figcaption", "dt", "dd", "dl", "hr", "body", "center", "main"};
  for (const char* b : kBlocks) {
    if (tag == b) return true;
  }
  return false;
}

bool is_skipped_tag(const std::string& tag) {
  return tag == "script" || tag == "style" || tag == "head" || tag == "title" ||
         tag == "meta" || tag == "link" || tag == "svg";
}

int heading_level(const std::string& tag) {
  if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') return tag[1] - '0';
  return 0;
}

// Collapses runs of whitespace the way HTML does, outside of <pre>.
void append_collapsed(std::string& out, const std::string& text) {
  for (char c : text) {
    bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    if (space) {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out += c;
    }
  }
}

struct ElementContext {
  std::string tag;
  InlineStyle style;
  bool block = false;
  bool hidden = false;
  int list_counter = 0;
  bool ordered = false;
  int quote_depth = 0;
};

bool is_vowel(uint32_t cp) {
  switch (cp) {
    case 'a': case 'e': case 'i': case 'o': case 'u': case 'y':
    case 'A': case 'E': case 'I': case 'O': case 'U': case 'Y':
      return true;
    default:
      return false;
  }
}

}  // namespace

std::vector<size_t> hyphenation_points(const std::string& word) {
  std::vector<size_t> points;
  // Honour existing breaks first: real hyphens and soft hyphens are always
  // safe places to split.
  size_t i = 0;
  std::vector<uint32_t> cps;
  std::vector<size_t> byte_at;
  while (i < word.size()) {
    byte_at.push_back(i);
    cps.push_back(utf8_next(word, i));
  }
  byte_at.push_back(word.size());
  for (size_t k = 0; k + 1 < cps.size(); ++k) {
    if (cps[k] == '-' || cps[k] == 0x00AD) points.push_back(byte_at[k + 1]);
  }
  if (!points.empty()) return points;

  // Otherwise a cautious vowel-consonant rule, only for long words, never
  // within three letters of either end. This avoids the worst artefacts of
  // pattern-free hyphenation while still loosening tight justified lines.
  if (cps.size() < 9) return points;
  for (size_t k = 3; k + 4 < cps.size(); ++k) {
    bool boundary = is_vowel(cps[k]) && !is_vowel(cps[k + 1]) && !is_vowel(cps[k + 2]);
    if (boundary) points.push_back(byte_at[k + 2]);
  }
  return points;
}

Document Layout::parse(int spine_index) const {
  Document doc;
  if (spine_index < 0 || spine_index >= (int)book_.spine().size()) return doc;
  doc.base_href = book_.spine()[spine_index].href;

  if (book_.format() == Book::Format::Cbz) {
    doc.is_image_only = true;
    Block b;
    b.kind = BlockKind::Image;
    b.image_href = book_.spine()[spine_index].href;
    doc.blocks.push_back(std::move(b));
    return doc;
  }

  std::string source;
  if (!book_.read_chapter(spine_index, source)) return doc;

  if (book_.format() == Book::Format::Text) {
    // Plain text: blank lines separate paragraphs, everything else is one
    // paragraph with its line breaks collapsed.
    std::string paragraph;
    auto flush = [&]() {
      std::string text = trim(paragraph);
      paragraph.clear();
      if (text.empty()) return;
      Block b;
      b.kind = BlockKind::Paragraph;
      b.frags.emplace_back(text, InlineStyle());
      doc.blocks.push_back(std::move(b));
    };
    std::vector<std::string> lines = split(source, '\n');
    for (std::string line : lines) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (trim(line).empty()) {
        flush();
      } else {
        if (!paragraph.empty()) paragraph += ' ';
        paragraph += trim(line);
      }
    }
    flush();
    return doc;
  }

  // ------------------------------------------------------------ XHTML path
  std::vector<ElementContext> stack;
  stack.push_back(ElementContext());

  Block current;
  bool current_open = false;
  std::string pending_text;
  bool in_pre = false;
  int list_depth = 0;
  std::string pending_id;

  auto style_top = [&]() -> InlineStyle& { return stack.back().style; };

  auto flush_text = [&]() {
    if (pending_text.empty()) return;
    if (!current_open) {
      current = Block();
      current.align = params_.align;
      current_open = true;
    }
    current.frags.emplace_back(pending_text, style_top());
    pending_text.clear();
  };

  auto finish_block = [&]() {
    flush_text();
    if (!current_open) return;
    bool has_text = false;
    for (const auto& f : current.frags) {
      if (!trim(f.first).empty()) {
        has_text = true;
        break;
      }
    }
    if (has_text || current.kind == BlockKind::Image || current.kind == BlockKind::Rule) {
      if (!current.id.empty()) doc.anchors[current.id] = (int)doc.blocks.size();
      doc.blocks.push_back(current);
    } else if (!current.id.empty()) {
      // Keep the anchor even for an empty block: footnote back-links and
      // chapter targets frequently point at one.
      doc.anchors[current.id] = (int)doc.blocks.size();
    }
    current = Block();
    current_open = false;
  };

  auto apply_declarations = [&](const std::vector<CssDeclaration>& decls, Block* block,
                                InlineStyle* inline_style, bool* hidden) {
    for (const CssDeclaration& d : decls) {
      const std::string& p = d.property;
      std::string v = to_lower(trim(d.value));
      if (p == "display" && v == "none") {
        if (hidden) *hidden = true;
      } else if (p == "font-weight") {
        if (inline_style) {
          inline_style->bold = v == "bold" || v == "bolder" || to_int(v) >= 600;
          if (v == "normal" || (to_int(v) > 0 && to_int(v) < 600)) inline_style->bold = false;
        }
      } else if (p == "font-style") {
        if (inline_style) inline_style->italic = v == "italic" || v == "oblique";
      } else if (p == "font-size") {
        float scale = 1.0f;
        if (inline_style && css_font_scale(v, scale)) inline_style->scale *= scale;
      } else if (p == "text-decoration" || p == "text-decoration-line") {
        if (inline_style) {
          if (v.find("underline") != std::string::npos) inline_style->underline = true;
          if (v.find("line-through") != std::string::npos) inline_style->strike = true;
          if (v.find("none") != std::string::npos) {
            inline_style->underline = false;
            inline_style->strike = false;
          }
        }
      } else if (p == "color") {
        Color c;
        if (inline_style && parse_css_color(d.value, c)) {
          inline_style->color = c;
          inline_style->has_color = true;
        }
      } else if (p == "text-align" && block) {
        if (v == "center") {
          block->centered = true;
          block->align = Alignment::Left;
          block->align_explicit = true;
        } else if (v == "justify") {
          block->align = Alignment::Justify;
          block->align_explicit = true;
        } else if (v == "left" || v == "start" || v == "right" || v == "end") {
          block->align = Alignment::Left;
          block->align_explicit = true;
        }
      } else if (p == "text-indent" && block) {
        int px = 0;
        if (css_length_px(v, params_.font_px, params_.content.w, params_.dpi, px)) {
          block->indent_px = std::max(0, std::min(params_.content.w / 2, px));
        }
      } else if ((p == "margin-top" || p == "padding-top") && block) {
        int px = 0;
        if (css_length_px(v, params_.font_px, params_.content.h, params_.dpi, px)) {
          block->space_before = std::max(0, std::min(params_.font_px * 3, px));
        }
      } else if ((p == "margin-bottom" || p == "padding-bottom") && block) {
        int px = 0;
        if (css_length_px(v, params_.font_px, params_.content.h, params_.dpi, px)) {
          block->space_after = std::max(0, std::min(params_.font_px * 3, px));
        }
      } else if ((p == "margin-left" || p == "padding-left") && block) {
        int px = 0;
        if (css_length_px(v, params_.font_px, params_.content.w, params_.dpi, px)) {
          block->left_padding = std::max(0, std::min(params_.content.w / 3, px));
        }
      } else if (p == "page-break-before" && block) {
        if (v == "always" || v == "left" || v == "right") block->page_break_before = true;
      }
    }
  };

  XmlParser parser(source);
  XmlParser::Token token;
  while ((token = parser.next()) != XmlParser::Token::End) {
    if (token == XmlParser::Token::Text) {
      if (stack.back().hidden) continue;
      if (in_pre) {
        pending_text += parser.text();
      } else {
        append_collapsed(pending_text, parser.text());
      }
      continue;
    }
    if (token == XmlParser::Token::StartTag) {
      const std::string tag = parser.tag();
      bool self_closing = parser.self_closing() || XmlParser::is_void_element(tag);

      if (is_skipped_tag(tag)) {
        if (!self_closing) {
          ElementContext ctx;
          ctx.tag = tag;
          ctx.style = style_top();
          ctx.hidden = true;
          stack.push_back(ctx);
        }
        continue;
      }

      // Text accumulated so far belongs to the *outer* style, so it has to
      // be committed before this element changes anything.
      flush_text();

      ElementContext ctx;
      ctx.tag = tag;
      ctx.style = style_top();
      ctx.hidden = stack.back().hidden;
      ctx.block = is_block_tag(tag);

      std::string id = parser.attr("id");
      std::string classes = parser.attr("class");

      if (tag == "br") {
        pending_text += '\n';
        continue;
      }
      if (tag == "hr") {
        finish_block();
        Block rule;
        rule.kind = BlockKind::Rule;
        rule.id = id;
        if (!id.empty()) doc.anchors[id] = (int)doc.blocks.size();
        doc.blocks.push_back(rule);
        continue;
      }
      if (tag == "img" || tag == "image") {
        std::string src = parser.attr("src");
        if (src.empty()) src = parser.attr("xlink:href");
        if (src.empty()) src = parser.attr("href");
        if (!src.empty() && !ctx.hidden) {
          finish_block();
          Block image;
          image.kind = BlockKind::Image;
          image.image_href = src;
          image.id = id;
          if (!id.empty()) doc.anchors[id] = (int)doc.blocks.size();
          doc.blocks.push_back(image);
        }
        continue;
      }

      // Inline emphasis.
      if (tag == "b" || tag == "strong") ctx.style.bold = true;
      if (tag == "i" || tag == "em" || tag == "cite" || tag == "dfn") ctx.style.italic = true;
      if (tag == "u" || tag == "ins") ctx.style.underline = true;
      if (tag == "s" || tag == "strike" || tag == "del") ctx.style.strike = true;
      if (tag == "code" || tag == "kbd" || tag == "samp" || tag == "tt") ctx.style.mono = true;
      if (tag == "sup") {
        ctx.style.superscript = true;
        ctx.style.scale *= 0.7f;
      }
      if (tag == "sub") {
        ctx.style.subscript = true;
        ctx.style.scale *= 0.7f;
      }
      if (tag == "small") ctx.style.scale *= 0.85f;
      if (tag == "big") ctx.style.scale *= 1.15f;
      if (tag == "a") {
        std::string href = parser.attr("href");
        if (!href.empty()) {
          doc.links.push_back(href);
          ctx.style.link = (int)doc.links.size() - 1;
          ctx.style.underline = true;
        }
      }

      if (ctx.block) {
        finish_block();
        current = Block();
        current.align = params_.align;
        current.id = id;
        current_open = true;

        int level = heading_level(tag);
        if (level > 0) {
          current.kind = BlockKind::Heading;
          current.heading_level = level;
          ctx.style.bold = true;
          // Headings step up in size: h1 largest, h6 barely larger than body.
          static const float kHeadingScale[7] = {1.0f, 1.7f, 1.45f, 1.28f, 1.15f, 1.05f, 1.0f};
          ctx.style.scale *= kHeadingScale[level];
          current.centered = level <= 2;
          current.align = Alignment::Left;
        } else if (tag == "li") {
          current.kind = BlockKind::ListItem;
          ElementContext* list = nullptr;
          for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            if (it->tag == "ul" || it->tag == "ol") {
              list = &(*it);
              break;
            }
          }
          if (list) {
            ++list->list_counter;
            current.marker = list->ordered ? format("%d.", list->list_counter) : "\xE2\x80\xA2";
          } else {
            current.marker = "\xE2\x80\xA2";
          }
          current.left_padding = params_.font_px * (1 + std::max(0, list_depth - 1));
        } else if (tag == "blockquote") {
          current.kind = BlockKind::Quote;
          current.left_padding = params_.font_px;
        } else if (tag == "pre") {
          current.kind = BlockKind::Pre;
          ctx.style.mono = true;
          in_pre = true;
        } else if (tag == "center") {
          current.centered = true;
        } else if (tag == "td" || tag == "th") {
          // Tables are laid out as stacked cells with a leading marker;
          // real column layout is out of scope, and stacking at least keeps
          // the content readable.
          current.left_padding = params_.font_px / 2;
          if (tag == "th") ctx.style.bold = true;
        }
        if (tag == "ul" || tag == "ol") {
          ctx.ordered = tag == "ol";
          ctx.list_counter = 0;
          ++list_depth;
          current_open = false;   // the list itself produces no block
        }
      }

      if (params_.honour_book_css) {
        std::vector<CssDeclaration> decls = book_.stylesheet().match(tag, classes, id);
        std::string inline_css = parser.attr("style");
        if (!inline_css.empty()) {
          std::vector<CssDeclaration> inline_decls =
              Stylesheet::parse_declarations(inline_css);
          decls.insert(decls.end(), inline_decls.begin(), inline_decls.end());
        }
        if (!decls.empty()) {
          apply_declarations(decls, current_open ? &current : nullptr, &ctx.style, &ctx.hidden);
        }
      }

      if (!self_closing) stack.push_back(ctx);
      continue;
    }
    if (token == XmlParser::Token::EndTag) {
      const std::string tag = parser.tag();
      if (tag == "pre") in_pre = false;
      if (tag == "ul" || tag == "ol") list_depth = std::max(0, list_depth - 1);
      if (is_block_tag(tag)) {
        finish_block();
      } else {
        // Commit this element's text before its style is popped.
        flush_text();
      }
      // Pop to the matching element; tolerate unbalanced markup.
      for (size_t i = stack.size(); i > 1; --i) {
        if (stack[i - 1].tag == tag) {
          stack.resize(i - 1);
          break;
        }
      }
      continue;
    }
  }
  finish_block();
  return doc;
}

// ---------------------------------------------------------------- line fill

std::vector<Line> Layout::layout_blocks(const Document& doc) const {
  std::vector<Line> lines;
  const int width = params_.content.w;
  const int base_px = params_.font_px;
  const int base_line_h =
      std::max(base_px + 2, base_px * params_.line_spacing / 100);

  // Resolve the reading font once; the italic and bold faces come from the
  // same family so metrics stay consistent.
  const std::string& family = params_.font_family;
  int y = 0;

  for (size_t bi = 0; bi < doc.blocks.size(); ++bi) {
    const Block& block = doc.blocks[bi];

    if (block.kind == BlockKind::Image) {
      Line line;
      line.block = (int)bi;
      line.block_start = true;
      line.image_href = block.image_href;
      const Canvas* img = image_for(doc, block.image_href);
      int w = width, h = base_line_h * 6;
      if (img && img->valid()) {
        double scale = std::min(1.0, (double)width / (double)img->width());
        // Never let one image exceed the page; the reader will scale it to
        // fit the remaining space when it places the page.
        int max_h = params_.content.h - base_line_h;
        w = std::max(1, (int)std::lround(img->width() * scale));
        h = std::max(1, (int)std::lround(img->height() * scale));
        if (h > max_h) {
          double shrink = (double)max_h / (double)h;
          w = std::max(1, (int)std::lround(w * shrink));
          h = max_h;
        }
      }
      line.image_width = w;
      line.height = h + base_line_h / 2;
      line.baseline = h;
      line.y = y;
      y += line.height;
      lines.push_back(std::move(line));
      continue;
    }

    if (block.kind == BlockKind::Rule) {
      Line line;
      line.block = (int)bi;
      line.block_start = true;
      line.rule = true;
      line.height = base_line_h;
      line.baseline = base_line_h / 2;
      line.y = y;
      y += line.height;
      lines.push_back(std::move(line));
      continue;
    }

    // Space above the block.
    int space_before = block.space_before;
    if (space_before < 0) {
      if (block.kind == BlockKind::Heading) {
        space_before = base_line_h;
      } else if (params_.extra_paragraph_spacing) {
        space_before = base_line_h / 2;
      } else {
        space_before = 0;
      }
    }
    y += space_before;

    int left = block.left_padding;
    int avail = std::max(base_px * 2, width - left);
    int indent = block.indent_px;
    if (indent < 0) {
      bool wants_indent =
          params_.force_indent && block.kind == BlockKind::Paragraph && !block.centered;
      indent = wants_indent ? base_px * 3 / 2 : 0;
    }

    // ---------------------------------------------------------- word walk
    struct Word {
      std::string text;
      InlineStyle style;
      int width = 0;
      int frag = 0;
      int offset = 0;
      bool space_before = false;
      bool forced_break = false;
    };
    std::vector<Word> words;
    bool pending_space = false;
    for (size_t fi = 0; fi < block.frags.size(); ++fi) {
      const std::string& text = block.frags[fi].first;
      const InlineStyle& style = block.frags[fi].second;
      TextStyle ts;
      ts.family = style.mono ? "Lexend Deca" : family;
      ts.px = std::max(8, (int)std::lround(base_px * style.scale));
      ts.style = style.bold ? (style.italic ? FontStyle::BoldItalic : FontStyle::Bold)
                            : (style.italic ? FontStyle::Italic : FontStyle::Regular);

      size_t i = 0;
      while (i < text.size()) {
        char c = text[i];
        if (c == '\n') {
          Word w;
          w.forced_break = true;
          w.frag = (int)fi;
          w.offset = (int)i;
          words.push_back(std::move(w));
          pending_space = false;
          ++i;
          continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
          pending_space = true;
          ++i;
          continue;
        }
        size_t end = i;
        while (end < text.size() && text[end] != ' ' && text[end] != '\n' &&
               text[end] != '\t' && text[end] != '\r') {
          ++end;
        }
        Word w;
        w.text = text.substr(i, end - i);
        w.style = style;
        w.frag = (int)fi;
        w.offset = (int)i;
        w.space_before = pending_space && !words.empty();
        w.width = text_width(w.text, ts);
        words.push_back(std::move(w));
        pending_space = false;
        i = end;
      }
    }

    // ------------------------------------------------------- fill lines
    size_t wi = 0;
    bool first_line = true;
    while (wi < words.size()) {
      Line line;
      line.block = (int)bi;
      line.block_start = first_line;
      line.frag = words[wi].frag;
      line.offset = words[wi].offset;
      int x = left + (first_line ? indent : 0);
      int line_max = left + avail;
      int max_px = base_px;
      bool forced_end = false;

      std::vector<Run> runs;
      while (wi < words.size()) {
        Word& w = words[wi];
        if (w.forced_break) {
          ++wi;
          forced_end = true;
          break;
        }
        TextStyle ts;
        ts.family = w.style.mono ? "Lexend Deca" : family;
        ts.px = std::max(8, (int)std::lround(base_px * w.style.scale));
        ts.style = w.style.bold ? (w.style.italic ? FontStyle::BoldItalic : FontStyle::Bold)
                                : (w.style.italic ? FontStyle::Italic : FontStyle::Regular);
        int space_w = (runs.empty() || !w.space_before) ? 0 : text_width(" ", ts);

        if (x + space_w + w.width > line_max && !runs.empty()) {
          // The word does not fit. Try to break it; otherwise end the line.
          bool split = false;
          if (params_.hyphenate && w.width > base_px * 3) {
            std::vector<size_t> points = hyphenation_points(w.text);
            for (auto it = points.rbegin(); it != points.rend() && !split; ++it) {
              if (*it == 0 || *it >= w.text.size()) continue;
              std::string head = w.text.substr(0, *it);
              std::string with_hyphen = head.back() == '-' ? head : head + "-";
              int head_w = text_width(with_hyphen, ts);
              if (x + space_w + head_w > line_max) continue;
              Run r;
              r.x = x + space_w;
              r.width = head_w;
              r.text = with_hyphen;
              r.style = w.style;
              r.space_before = w.space_before && !runs.empty();
              runs.push_back(r);
              x = r.x + head_w;
              max_px = std::max(max_px, ts.px);
              // The tail starts the next line.
              w.text = w.text.substr(*it);
              w.offset += (int)*it;
              w.width = text_width(w.text, ts);
              w.space_before = false;
              split = true;
            }
          }
          break;
        }

        Run r;
        r.x = x + space_w;
        r.width = w.width;
        r.text = w.text;
        r.style = w.style;
        r.space_before = space_w > 0;
        runs.push_back(r);
        x = r.x + w.width;
        max_px = std::max(max_px, ts.px);
        ++wi;
      }

      if (runs.empty() && !forced_end) {
        // A single word wider than the line: place it anyway so we make
        // progress instead of looping forever.
        if (wi < words.size()) {
          Word& w = words[wi];
          Run r;
          r.x = x;
          r.width = w.width;
          r.text = w.text;
          r.style = w.style;
          runs.push_back(r);
          ++wi;
        } else {
          break;
        }
      }

      int line_height = std::max(base_line_h, max_px * params_.line_spacing / 100);
      line.height = line_height;
      line.baseline = (int)std::lround(line_height * 0.78);
      line.y = y;

      bool last_line = wi >= words.size() || forced_end;
      Alignment align = block.align_explicit ? block.align : params_.align;
      if (block.centered) {
        int content_w = runs.empty() ? 0 : runs.back().x + runs.back().width - runs.front().x;
        int shift = (avail - content_w) / 2 - (runs.empty() ? 0 : runs.front().x - left);
        for (Run& r : runs) r.x += shift;
      } else if (align == Alignment::Justify && !last_line && runs.size() > 1) {
        int content_w = runs.back().x + runs.back().width - runs.front().x;
        int slack = avail - (runs.front().x - left) - content_w;
        // Only stretch by a sane amount; a very short line looks worse
        // justified than ragged.
        int gaps = 0;
        for (size_t k = 1; k < runs.size(); ++k) {
          if (runs[k].space_before) ++gaps;
        }
        if (slack > 0 && gaps > 0 && slack < avail / 4) {
          int distributed = 0;
          int seen = 0;
          for (size_t k = 1; k < runs.size(); ++k) {
            if (runs[k].space_before) {
              ++seen;
              distributed = (int)std::lround((double)slack * (double)seen / (double)gaps);
            }
            runs[k].x += distributed;
          }
        }
      }

      // The list marker hangs in the left padding of the first line.
      if (first_line && !block.marker.empty()) {
        TextStyle ts;
        ts.family = family;
        ts.px = base_px;
        Run marker;
        marker.text = block.marker;
        marker.width = text_width(block.marker, ts);
        marker.x = std::max(0, left - marker.width - base_px / 3);
        marker.style = block.frags.empty() ? InlineStyle() : block.frags.front().second;
        marker.style.link = -1;
        runs.insert(runs.begin(), marker);
      }

      line.runs = std::move(runs);
      y += line_height;
      lines.push_back(std::move(line));
      first_line = false;
    }

    int space_after = block.space_after;
    if (space_after < 0) {
      if (block.kind == BlockKind::Heading) {
        space_after = base_line_h / 2;
      } else if (params_.extra_paragraph_spacing) {
        space_after = base_line_h / 2;
      } else {
        space_after = 0;
      }
    }
    y += space_after;
  }
  return lines;
}

std::vector<Page> Layout::paginate(const Document& doc) const {
  std::vector<Line> lines = layout_blocks(doc);
  std::vector<Page> pages;
  const int page_h = params_.content.h;
  if (lines.empty()) {
    pages.push_back(Page());
    return pages;
  }

  Page page;
  int page_top = lines.front().y;
  page.start_block = lines.front().block;
  page.start_frag = lines.front().frag;
  page.start_offset = lines.front().offset;

  for (size_t i = 0; i < lines.size(); ++i) {
    Line line = lines[i];
    bool page_break = doc.blocks.size() > (size_t)line.block &&
                      doc.blocks[line.block].page_break_before && line.block_start &&
                      !page.lines.empty();
    int local_y = line.y - page_top;
    if (page_break || (local_y + line.height > page_h && !page.lines.empty())) {
      page.height = page_h;
      pages.push_back(std::move(page));
      page = Page();
      page_top = line.y;
      page.start_block = line.block;
      page.start_frag = line.frag;
      page.start_offset = line.offset;
      local_y = 0;
    }
    line.y = local_y;
    page.lines.push_back(std::move(line));
  }
  if (!page.lines.empty()) {
    page.height = page_h;
    pages.push_back(std::move(page));
  }
  return pages;
}

int Layout::page_for_position(const std::vector<Page>& pages, int block, int frag, int offset) {
  int best = 0;
  for (size_t i = 0; i < pages.size(); ++i) {
    const Page& p = pages[i];
    bool before = p.start_block < block ||
                  (p.start_block == block &&
                   (p.start_frag < frag || (p.start_frag == frag && p.start_offset <= offset)));
    if (before) {
      best = (int)i;
    } else {
      break;
    }
  }
  return best;
}

const Canvas* Layout::image_for(const Document& doc, const std::string& href) const {
  auto it = image_cache_.find(href);
  if (it != image_cache_.end()) return it->second.get();
  std::string data;
  std::shared_ptr<Canvas> canvas;
  if (book_.read_resource(href, doc.base_href, data)) {
    canvas = std::make_shared<Canvas>();
    if (!Canvas::decode_image(data.data(), data.size(), *canvas)) {
      canvas.reset();
    } else if (!params_.color_images) {
      canvas->to_grayscale();
    }
  }
  // Cache misses too: a broken image reference should be looked up once.
  image_cache_[href] = canvas;
  // Keep the cache from growing without bound on image-heavy books.
  if (image_cache_.size() > 24) {
    for (auto i = image_cache_.begin(); i != image_cache_.end();) {
      if (i->first != href) {
        i = image_cache_.erase(i);
        if (image_cache_.size() <= 12) break;
      } else {
        ++i;
      }
    }
  }
  return image_cache_[href].get();
}

void Layout::draw(Canvas& canvas, const Page& page, const Document& doc,
                  std::vector<std::pair<Rect, int>>* link_rects) const {
  const Rect& content = params_.content;
  for (const Line& line : page.lines) {
    int baseline_y = content.y + line.y + line.baseline;

    if (!line.image_href.empty()) {
      const Canvas* img = image_for(doc, line.image_href);
      int h = line.baseline;
      int w = line.image_width;
      Rect dst(content.x + (content.w - w) / 2, content.y + line.y, w, h);
      if (img && img->valid()) {
        canvas.blit_scaled(*img, dst);
      } else {
        // Placeholder so the reader can see something is missing.
        canvas.draw_rect(dst, Color::gray(180), 2);
      }
      continue;
    }
    if (line.rule) {
      int y = content.y + line.y + line.height / 2;
      canvas.fill_rect(Rect(content.x + content.w / 4, y, content.w / 2, 2), Color::gray(140));
      continue;
    }

    for (const Run& run : line.runs) {
      TextStyle ts;
      ts.family = run.style.mono ? "Lexend Deca" : params_.font_family;
      ts.px = std::max(8, (int)std::lround(params_.font_px * run.style.scale));
      ts.style = run.style.bold ? (run.style.italic ? FontStyle::BoldItalic : FontStyle::Bold)
                                : (run.style.italic ? FontStyle::Italic : FontStyle::Regular);
      ts.color = run.style.has_color ? run.style.color : params_.text_color;
      ts.underline = run.style.underline;
      ts.strikethrough = run.style.strike;

      int y = baseline_y;
      if (run.style.superscript) y -= ts.px / 2;
      if (run.style.subscript) y += ts.px / 4;
      draw_text(canvas, content.x + run.x, y, run.text, ts);

      if (link_rects && run.style.link >= 0) {
        link_rects->emplace_back(
            Rect(content.x + run.x, content.y + line.y, run.width, line.height),
            run.style.link);
      }
    }
  }
}

}  // namespace ck
