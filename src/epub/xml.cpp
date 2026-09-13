#include "epub/xml.h"

#include <cctype>

#include "core/str.h"

namespace ck {

bool XmlParser::is_void_element(const std::string& tag) {
  static const char* kVoid[] = {"area", "base",  "br",   "col",  "embed",  "hr",
                                "img",  "input", "link", "meta", "param",  "source",
                                "track", "wbr",  "image"};
  for (const char* v : kVoid) {
    if (tag == v) return true;
  }
  return false;
}

std::string XmlParser::attr(const std::string& name) const {
  for (const Attr& a : attrs_) {
    if (a.name == name) return a.value;
  }
  return "";
}

bool XmlParser::has_attr(const std::string& name) const {
  for (const Attr& a : attrs_) {
    if (a.name == name) return true;
  }
  return false;
}

void XmlParser::parse_tag(size_t start) {
  attrs_.clear();
  self_closing_ = false;
  size_t i = start;  // points just past '<'
  bool closing = false;
  if (i < text_.size() && text_[i] == '/') {
    closing = true;
    ++i;
  }
  size_t name_start = i;
  while (i < text_.size() && !isspace((unsigned char)text_[i]) && text_[i] != '>' &&
         text_[i] != '/') {
    ++i;
  }
  tag_ = to_lower(text_.substr(name_start, i - name_start));

  while (i < text_.size() && text_[i] != '>') {
    while (i < text_.size() && (isspace((unsigned char)text_[i]) || text_[i] == '/')) {
      if (text_[i] == '/') self_closing_ = true;
      ++i;
    }
    if (i >= text_.size() || text_[i] == '>') break;
    size_t attr_start = i;
    while (i < text_.size() && text_[i] != '=' && text_[i] != '>' &&
           !isspace((unsigned char)text_[i])) {
      ++i;
    }
    Attr a;
    a.name = to_lower(text_.substr(attr_start, i - attr_start));
    while (i < text_.size() && isspace((unsigned char)text_[i])) ++i;
    if (i < text_.size() && text_[i] == '=') {
      ++i;
      while (i < text_.size() && isspace((unsigned char)text_[i])) ++i;
      if (i < text_.size() && (text_[i] == '"' || text_[i] == '\'')) {
        char quote = text_[i++];
        size_t value_start = i;
        while (i < text_.size() && text_[i] != quote) ++i;
        a.value = decode_entities(text_.substr(value_start, i - value_start));
        if (i < text_.size()) ++i;
      } else {
        size_t value_start = i;
        while (i < text_.size() && !isspace((unsigned char)text_[i]) && text_[i] != '>') ++i;
        a.value = decode_entities(text_.substr(value_start, i - value_start));
      }
    }
    if (!a.name.empty()) attrs_.push_back(std::move(a));
  }
  if (i < text_.size()) ++i;  // consume '>'
  pos_ = i;
  if (closing) self_closing_ = false;
  if (!closing && (tag_ == "script" || tag_ == "style")) in_raw_text_ = !self_closing_;
  tag_is_closing_ = closing;
}

XmlParser::Token XmlParser::next() {
  if (pos_ >= text_.size()) return Token::End;

  // Inside <script>/<style> everything up to the matching end tag is raw
  // text that must not be parsed as markup.
  if (in_raw_text_) {
    std::string end = "</" + tag_;
    size_t close = to_lower(text_).find(end, pos_);
    if (close == std::string::npos) {
      pos_ = text_.size();
      in_raw_text_ = false;
      value_.clear();
      return Token::Text;
    }
    value_ = text_.substr(pos_, close - pos_);
    pos_ = close;
    in_raw_text_ = false;
    return Token::Text;
  }

  if (text_[pos_] == '<') {
    if (text_.compare(pos_, 4, "<!--") == 0) {
      size_t close = text_.find("-->", pos_ + 4);
      value_ = text_.substr(pos_ + 4, close == std::string::npos ? std::string::npos
                                                                 : close - pos_ - 4);
      pos_ = close == std::string::npos ? text_.size() : close + 3;
      return Token::Comment;
    }
    if (text_.compare(pos_, 9, "<![CDATA[") == 0) {
      size_t close = text_.find("]]>", pos_ + 9);
      value_ = text_.substr(pos_ + 9, close == std::string::npos ? std::string::npos
                                                                : close - pos_ - 9);
      pos_ = close == std::string::npos ? text_.size() : close + 3;
      return Token::Text;
    }
    if (text_[pos_ + 1] == '!' || text_[pos_ + 1] == '?') {
      size_t close = text_.find('>', pos_);
      value_ = text_.substr(pos_, close == std::string::npos ? std::string::npos
                                                            : close - pos_ + 1);
      pos_ = close == std::string::npos ? text_.size() : close + 1;
      return Token::Declaration;
    }
    parse_tag(pos_ + 1);
    if (tag_.empty()) {
      // A stray '<' in character data.
      value_ = "<";
      return Token::Text;
    }
    return tag_is_closing_ ? Token::EndTag : Token::StartTag;
  }

  size_t next_tag = text_.find('<', pos_);
  if (next_tag == std::string::npos) next_tag = text_.size();
  value_ = decode_entities(text_.substr(pos_, next_tag - pos_));
  pos_ = next_tag;
  return Token::Text;
}

std::string xml_text_content(const std::string& fragment) {
  XmlParser p(fragment);
  std::string out;
  XmlParser::Token t;
  while ((t = p.next()) != XmlParser::Token::End) {
    if (t == XmlParser::Token::Text) out += p.text();
  }
  return out;
}

}  // namespace ck
