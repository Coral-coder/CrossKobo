#include "epub/css.h"

#include <algorithm>
#include <cmath>
#include <cctype>

#include "core/str.h"

namespace ck {
namespace {

// Strips comments and normalises whitespace so the tiny parser below can
// stay simple.
std::string strip_comments(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in.compare(i, 2, "/*") == 0) {
      size_t end = in.find("*/", i + 2);
      i = end == std::string::npos ? in.size() : end + 2;
      out += ' ';
      continue;
    }
    out += in[i++];
  }
  return out;
}

CssSelector parse_simple_selector(const std::string& raw) {
  CssSelector sel;
  // Only the right-most compound selector is honoured; a descendant
  // combinator is treated as "applies wherever this element appears".
  std::string last = raw;
  size_t sp = last.find_last_of(" >+~\t\n");
  if (sp != std::string::npos) last = last.substr(sp + 1);
  last = trim(last);

  size_t i = 0;
  std::string current;
  char kind = 't';
  auto commit = [&]() {
    if (current.empty()) return;
    if (kind == 't') {
      sel.tag = to_lower(current);
      sel.specificity += 1;
    } else if (kind == '.') {
      sel.klass = current;
      sel.specificity += 10;
    } else if (kind == '#') {
      sel.id = current;
      sel.specificity += 100;
    }
    current.clear();
  };
  while (i < last.size()) {
    char c = last[i];
    if (c == '.' || c == '#') {
      commit();
      kind = c;
      ++i;
      continue;
    }
    if (c == ':' || c == '[') break;  // pseudo-classes and attribute selectors
    current += c;
    ++i;
  }
  commit();
  if (sel.tag == "*") sel.tag.clear();
  return sel;
}

}  // namespace

std::vector<CssDeclaration> Stylesheet::parse_declarations(const std::string& text) {
  std::vector<CssDeclaration> out;
  for (const std::string& part : split(text, ';')) {
    size_t colon = part.find(':');
    if (colon == std::string::npos) continue;
    CssDeclaration d;
    d.property = to_lower(trim(part.substr(0, colon)));
    d.value = trim(part.substr(colon + 1));
    // Drop !important markers; we have no cascade to fight over.
    size_t bang = d.value.find('!');
    if (bang != std::string::npos) d.value = trim(d.value.substr(0, bang));
    if (!d.property.empty() && !d.value.empty()) out.push_back(std::move(d));
  }
  return out;
}

void Stylesheet::parse(const std::string& raw) {
  std::string text = strip_comments(raw);
  size_t i = 0;
  while (i < text.size()) {
    size_t brace = text.find('{', i);
    if (brace == std::string::npos) break;
    std::string selectors = trim(text.substr(i, brace - i));
    size_t close = text.find('}', brace);
    if (close == std::string::npos) break;
    std::string body = text.substr(brace + 1, close - brace - 1);
    i = close + 1;

    // Skip at-rules: @media blocks would need a media query engine, and
    // @font-face needs embedded font support we do not have yet.
    if (!selectors.empty() && selectors[0] == '@') {
      // For @media, parse the inner rules anyway - most books use it for
      // print or small-screen tweaks that are harmless here.
      if (starts_with(to_lower(selectors), "@media")) {
        parse(body);
      }
      continue;
    }
    if (selectors.empty()) continue;

    CssRule rule;
    rule.order = next_order_++;
    rule.declarations = parse_declarations(body);
    if (rule.declarations.empty()) continue;
    for (const std::string& sel : split(selectors, ',')) {
      std::string s = trim(sel);
      if (s.empty()) continue;
      rule.selectors.push_back(parse_simple_selector(s));
    }
    if (!rule.selectors.empty()) rules_.push_back(std::move(rule));
  }
}

std::vector<CssDeclaration> Stylesheet::match(const std::string& tag, const std::string& classes,
                                              const std::string& id) const {
  std::vector<std::string> class_list = split(classes, ' ');
  struct Hit {
    int specificity;
    int order;
    const CssRule* rule;
  };
  std::vector<Hit> hits;
  for (const CssRule& rule : rules_) {
    int best = -1;
    for (const CssSelector& sel : rule.selectors) {
      if (!sel.tag.empty() && sel.tag != tag) continue;
      if (!sel.id.empty() && sel.id != id) continue;
      if (!sel.klass.empty()) {
        bool found = false;
        for (const std::string& c : class_list) {
          if (c == sel.klass) {
            found = true;
            break;
          }
        }
        if (!found) continue;
      }
      best = std::max(best, sel.specificity);
    }
    if (best >= 0) hits.push_back({best, rule.order, &rule});
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    if (a.specificity != b.specificity) return a.specificity < b.specificity;
    return a.order < b.order;
  });
  std::vector<CssDeclaration> out;
  for (const Hit& h : hits) {
    out.insert(out.end(), h.rule->declarations.begin(), h.rule->declarations.end());
  }
  return out;
}

bool css_length_px(const std::string& value, int em_px, int percent_base, int dpi, int& out) {
  std::string v = to_lower(trim(value));
  if (v.empty()) return false;
  if (v == "0") {
    out = 0;
    return true;
  }
  if (v == "auto" || v == "inherit" || v == "initial") return false;
  double num = to_double(v, 0.0);
  auto ends = [&](const char* suffix) { return ends_with(v, suffix); };
  if (ends("px")) {
    // Book CSS pixels assume a ~96 dpi screen; scale to the panel so a
    // 20px indent is the same physical size everywhere.
    out = (int)std::lround(num * (double)dpi / 96.0);
  } else if (ends("em")) {
    out = (int)std::lround(num * em_px);
  } else if (ends("rem")) {
    out = (int)std::lround(num * em_px);
  } else if (ends("ex")) {
    out = (int)std::lround(num * em_px * 0.5);
  } else if (ends("%")) {
    out = (int)std::lround(num * percent_base / 100.0);
  } else if (ends("pt")) {
    out = (int)std::lround(num * (double)dpi / 72.0);
  } else if (ends("mm")) {
    out = (int)std::lround(num * (double)dpi / 25.4);
  } else if (ends("cm")) {
    out = (int)std::lround(num * (double)dpi / 2.54);
  } else if (ends("in")) {
    out = (int)std::lround(num * (double)dpi);
  } else {
    // Unitless: treat as CSS pixels.
    out = (int)std::lround(num * (double)dpi / 96.0);
  }
  return true;
}

bool css_font_scale(const std::string& value, float& out) {
  std::string v = to_lower(trim(value));
  if (v.empty()) return false;
  struct Keyword {
    const char* name;
    float scale;
  };
  static const Keyword kKeywords[] = {
      {"xx-small", 0.6f}, {"x-small", 0.75f}, {"small", 0.89f},  {"medium", 1.0f},
      {"large", 1.2f},    {"x-large", 1.5f},  {"xx-large", 2.0f}, {"smaller", 0.85f},
      {"larger", 1.18f},
  };
  for (const Keyword& k : kKeywords) {
    if (v == k.name) {
      out = k.scale;
      return true;
    }
  }
  double num = to_double(v, 0.0);
  if (num <= 0.0) return false;
  if (ends_with(v, "em") || ends_with(v, "rem")) {
    out = (float)num;
  } else if (ends_with(v, "%")) {
    out = (float)(num / 100.0);
  } else if (ends_with(v, "pt")) {
    out = (float)(num / 12.0);
  } else if (ends_with(v, "px")) {
    out = (float)(num / 16.0);
  } else {
    return false;
  }
  // Keep books from making text unreadably small or absurdly large.
  out = std::max(0.5f, std::min(3.0f, out));
  return true;
}

}  // namespace ck
