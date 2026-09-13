#pragma once
#include <string>
#include <vector>

#include "gfx/color.h"

namespace ck {

// A deliberately small CSS subset: the declarations that change how a book
// reads (weight, slant, alignment, indents, spacing, colour, visibility)
// matched against simple selectors. Anything else is ignored rather than
// half-honoured, which keeps typography predictable.
struct CssDeclaration {
  std::string property;
  std::string value;
};

struct CssSelector {
  std::string tag;    // "p", empty = any
  std::string klass;  // without the dot
  std::string id;     // without the hash
  int specificity = 0;
};

struct CssRule {
  std::vector<CssSelector> selectors;
  std::vector<CssDeclaration> declarations;
  int order = 0;
};

class Stylesheet {
 public:
  void parse(const std::string& text);
  void clear() { rules_.clear(); }
  bool empty() const { return rules_.empty(); }

  // Declarations that apply to an element, weakest first, so a caller can
  // apply them in order and let later ones win.
  std::vector<CssDeclaration> match(const std::string& tag, const std::string& classes,
                                    const std::string& id) const;
  // Parses an inline style="..." attribute.
  static std::vector<CssDeclaration> parse_declarations(const std::string& text);

 private:
  std::vector<CssRule> rules_;
  int next_order_ = 0;
};

// --------------------------------------------------------------- value help
// Converts a CSS length to pixels. `em_px` is the current font size and
// `percent_base` the containing width, both needed because books mix units
// freely. Returns false for values we cannot interpret.
bool css_length_px(const std::string& value, int em_px, int percent_base, int dpi, int& out);
// Font size as a multiplier of the inherited size ("1.2em", "120%", "large").
bool css_font_scale(const std::string& value, float& out);

}  // namespace ck
