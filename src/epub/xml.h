#pragma once
#include <string>
#include <vector>

namespace ck {

// A forgiving XML/XHTML pull parser. EPUB content is nominally XHTML but in
// practice ranges from strict XML to hand-written HTML with unquoted
// attributes and unclosed tags, so this parser never fails: it just returns
// the best interpretation of what it was given.
class XmlParser {
 public:
  enum class Token { End, StartTag, EndTag, Text, Comment, Declaration };

  struct Attr {
    std::string name;   // lowercased
    std::string value;  // entity-decoded
  };

  explicit XmlParser(const std::string& text) : text_(text) {}

  Token next();
  // Valid after next() returned StartTag or EndTag.
  const std::string& tag() const { return tag_; }
  bool self_closing() const { return self_closing_; }
  const std::vector<Attr>& attrs() const { return attrs_; }
  std::string attr(const std::string& name) const;
  bool has_attr(const std::string& name) const;
  // Valid after next() returned Text: entity-decoded character data.
  const std::string& text() const { return value_; }

  size_t offset() const { return pos_; }

  // True for HTML elements that never have an end tag.
  static bool is_void_element(const std::string& tag);

 private:
  void parse_tag(size_t start);

  const std::string& text_;
  size_t pos_ = 0;
  std::string tag_;
  std::string value_;
  std::vector<Attr> attrs_;
  bool self_closing_ = false;
  bool tag_is_closing_ = false;
  bool in_raw_text_ = false;   // inside <script>/<style>
};

// Convenience: strip all markup from a fragment, keeping character data.
std::string xml_text_content(const std::string& fragment);

}  // namespace ck
