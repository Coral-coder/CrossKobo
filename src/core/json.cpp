#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/fs.h"
#include "core/str.h"

namespace ck {
namespace {

const Json kNull;

void escape_into(const std::string& s, std::string& out) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += (char)c;
        }
    }
  }
  out += '"';
}

struct Parser {
  const std::string& s;
  size_t i = 0;
  std::string error;

  explicit Parser(const std::string& text) : s(text) {}

  void skip_ws() {
    while (i < s.size()) {
      char c = s[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++i;
      } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
        // Tolerate // comments: people do hand-edit these files.
        while (i < s.size() && s[i] != '\n') ++i;
      } else {
        break;
      }
    }
  }

  bool fail(const std::string& msg) {
    if (error.empty()) error = format("%s at offset %zu", msg.c_str(), i);
    return false;
  }

  bool parse_value(Json& out) {
    skip_ws();
    if (i >= s.size()) return fail("unexpected end of input");
    char c = s[i];
    switch (c) {
      case '{': return parse_object(out);
      case '[': return parse_array(out);
      case '"': {
        std::string str;
        if (!parse_string(str)) return false;
        out = Json(std::move(str));
        return true;
      }
      case 't':
        if (s.compare(i, 4, "true") == 0) {
          i += 4;
          out = Json(true);
          return true;
        }
        return fail("invalid token");
      case 'f':
        if (s.compare(i, 5, "false") == 0) {
          i += 5;
          out = Json(false);
          return true;
        }
        return fail("invalid token");
      case 'n':
        if (s.compare(i, 4, "null") == 0) {
          i += 4;
          out = Json();
          return true;
        }
        return fail("invalid token");
      default: return parse_number(out);
    }
  }

  bool parse_number(Json& out) {
    const char* start = s.c_str() + i;
    char* end = nullptr;
    double v = strtod(start, &end);
    if (end == start) return fail("invalid number");
    i += (size_t)(end - start);
    out = Json(v);
    return true;
  }

  bool parse_string(std::string& out) {
    if (i >= s.size() || s[i] != '"') return fail("expected string");
    ++i;
    out.clear();
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) break;
      char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (i + 4 > s.size()) return fail("truncated \\u escape");
          uint32_t cp = (uint32_t)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
          i += 4;
          // Surrogate pair.
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' &&
              s[i + 1] == 'u') {
            uint32_t low = (uint32_t)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
            if (low >= 0xDC00 && low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
              i += 6;
            }
          }
          utf8_append(out, cp);
          break;
        }
        default: return fail("invalid escape");
      }
    }
    return fail("unterminated string");
  }

  bool parse_array(Json& out) {
    ++i;  // '['
    out = Json::array();
    skip_ws();
    if (i < s.size() && s[i] == ']') {
      ++i;
      return true;
    }
    while (true) {
      Json v;
      if (!parse_value(v)) return false;
      out.push_back(std::move(v));
      skip_ws();
      if (i >= s.size()) return fail("unterminated array");
      if (s[i] == ',') {
        ++i;
        continue;
      }
      if (s[i] == ']') {
        ++i;
        return true;
      }
      return fail("expected , or ]");
    }
  }

  bool parse_object(Json& out) {
    ++i;  // '{'
    out = Json::object();
    skip_ws();
    if (i < s.size() && s[i] == '}') {
      ++i;
      return true;
    }
    while (true) {
      skip_ws();
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (i >= s.size() || s[i] != ':') return fail("expected :");
      ++i;
      Json v;
      if (!parse_value(v)) return false;
      out[key] = std::move(v);
      skip_ws();
      if (i >= s.size()) return fail("unterminated object");
      if (s[i] == ',') {
        ++i;
        continue;
      }
      if (s[i] == '}') {
        ++i;
        return true;
      }
      return fail("expected , or }");
    }
  }
};

}  // namespace

bool Json::as_bool(bool fallback) const {
  if (type_ == Type::Bool) return bool_;
  if (type_ == Type::Number) return num_ != 0.0;
  if (type_ == Type::String) return str_ == "true" || str_ == "1";
  return fallback;
}

double Json::as_double(double fallback) const {
  if (type_ == Type::Number) return num_;
  if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
  if (type_ == Type::String) return to_double(str_, fallback);
  return fallback;
}

int Json::as_int(int fallback) const { return (int)llround(as_double((double)fallback)); }
int64_t Json::as_int64(int64_t fallback) const {
  return (int64_t)llround(as_double((double)fallback));
}

std::string Json::as_string(const std::string& fallback) const {
  if (type_ == Type::String) return str_;
  if (type_ == Type::Number) {
    if (num_ == (double)(int64_t)num_) return format("%lld", (long long)num_);
    return format("%g", num_);
  }
  if (type_ == Type::Bool) return bool_ ? "true" : "false";
  return fallback;
}

const Json* Json::find(const std::string& key) const {
  for (const auto& kv : object_) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

Json& Json::operator[](const std::string& key) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    object_.clear();
  }
  for (auto& kv : object_) {
    if (kv.first == key) return kv.second;
  }
  object_.emplace_back(key, Json());
  return object_.back().second;
}

void Json::erase(const std::string& key) {
  for (size_t i = 0; i < object_.size(); ++i) {
    if (object_[i].first == key) {
      object_.erase(object_.begin() + (long)i);
      return;
    }
  }
}

bool Json::get_bool(const std::string& key, bool fallback) const {
  const Json* v = find(key);
  return v ? v->as_bool(fallback) : fallback;
}
int Json::get_int(const std::string& key, int fallback) const {
  const Json* v = find(key);
  return v ? v->as_int(fallback) : fallback;
}
int64_t Json::get_int64(const std::string& key, int64_t fallback) const {
  const Json* v = find(key);
  return v ? v->as_int64(fallback) : fallback;
}
double Json::get_double(const std::string& key, double fallback) const {
  const Json* v = find(key);
  return v ? v->as_double(fallback) : fallback;
}
std::string Json::get_string(const std::string& key, const std::string& fallback) const {
  const Json* v = find(key);
  return v ? v->as_string(fallback) : fallback;
}

size_t Json::size() const {
  if (type_ == Type::Array) return array_.size();
  if (type_ == Type::Object) return object_.size();
  return 0;
}

const Json& Json::at(size_t i) const {
  if (type_ != Type::Array || i >= array_.size()) return kNull;
  return array_[i];
}

Json& Json::at(size_t i) { return array_[i]; }

void Json::push_back(Json v) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    array_.clear();
  }
  array_.push_back(std::move(v));
}

void Json::dump_to(std::string& out, bool pretty, int indent) const {
  auto newline = [&](int depth) {
    if (!pretty) return;
    out += '\n';
    out.append((size_t)depth * 2, ' ');
  };
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
      if (num_ == (double)(int64_t)num_ && std::fabs(num_) < 9.2e18) {
        out += format("%lld", (long long)num_);
      } else {
        out += format("%.10g", num_);
      }
      break;
    }
    case Type::String: escape_into(str_, out); break;
    case Type::Array: {
      if (array_.empty()) {
        out += "[]";
        break;
      }
      out += '[';
      for (size_t i = 0; i < array_.size(); ++i) {
        if (i) out += ',';
        newline(indent + 1);
        array_[i].dump_to(out, pretty, indent + 1);
      }
      newline(indent);
      out += ']';
      break;
    }
    case Type::Object: {
      if (object_.empty()) {
        out += "{}";
        break;
      }
      out += '{';
      for (size_t i = 0; i < object_.size(); ++i) {
        if (i) out += ',';
        newline(indent + 1);
        escape_into(object_[i].first, out);
        out += pretty ? ": " : ":";
        object_[i].second.dump_to(out, pretty, indent + 1);
      }
      newline(indent);
      out += '}';
      break;
    }
  }
}

std::string Json::dump(bool pretty) const {
  std::string out;
  dump_to(out, pretty, 0);
  return out;
}

bool Json::parse(const std::string& text, Json& out, std::string* error) {
  Parser p(text);
  if (!p.parse_value(out)) {
    if (error) *error = p.error;
    return false;
  }
  return true;
}

bool Json::parse_file(const std::string& path, Json& out) {
  std::string text;
  if (!fs::read_file(path, text)) return false;
  return parse(text, out, nullptr);
}

bool Json::save_file(const std::string& path, bool pretty) const {
  return fs::write_file_atomic(path, dump(pretty) + "\n");
}

}  // namespace ck
