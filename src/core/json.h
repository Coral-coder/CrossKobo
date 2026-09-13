#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ck {

// A small dependency-free JSON tree. Used for settings, per-book state,
// reading statistics and notebook metadata. Object keys keep insertion order
// so a hand-edited settings file stays recognisable after a rewrite.
class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  explicit Json(bool b) : type_(Type::Bool), bool_(b) {}
  explicit Json(double n) : type_(Type::Number), num_(n) {}
  explicit Json(int n) : type_(Type::Number), num_(n) {}
  explicit Json(int64_t n) : type_(Type::Number), num_((double)n) {}
  explicit Json(const char* s) : type_(Type::String), str_(s) {}
  explicit Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

  static Json array() {
    Json j;
    j.type_ = Type::Array;
    return j;
  }
  static Json object() {
    Json j;
    j.type_ = Type::Object;
    return j;
  }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_object() const { return type_ == Type::Object; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_string() const { return type_ == Type::String; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_bool() const { return type_ == Type::Bool; }

  // Typed reads with a fallback, so a corrupt or partial file degrades to
  // defaults instead of failing to load.
  bool as_bool(bool fallback = false) const;
  double as_double(double fallback = 0.0) const;
  int as_int(int fallback = 0) const;
  int64_t as_int64(int64_t fallback = 0) const;
  std::string as_string(const std::string& fallback = "") const;

  // Object access. get() never creates; operator[] does (for building).
  const Json* find(const std::string& key) const;
  Json& operator[](const std::string& key);
  bool has(const std::string& key) const { return find(key) != nullptr; }
  void erase(const std::string& key);
  const std::vector<std::pair<std::string, Json>>& members() const { return object_; }

  bool get_bool(const std::string& key, bool fallback = false) const;
  int get_int(const std::string& key, int fallback = 0) const;
  int64_t get_int64(const std::string& key, int64_t fallback = 0) const;
  double get_double(const std::string& key, double fallback = 0.0) const;
  std::string get_string(const std::string& key, const std::string& fallback = "") const;

  // Array access.
  size_t size() const;
  const Json& at(size_t i) const;
  Json& at(size_t i);
  void push_back(Json v);
  const std::vector<Json>& items() const { return array_; }

  std::string dump(bool pretty = false) const;
  static bool parse(const std::string& text, Json& out, std::string* error = nullptr);
  static bool parse_file(const std::string& path, Json& out);
  bool save_file(const std::string& path, bool pretty = true) const;

 private:
  void dump_to(std::string& out, bool pretty, int indent) const;

  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0.0;
  std::string str_;
  std::vector<Json> array_;
  std::vector<std::pair<std::string, Json>> object_;
};

}  // namespace ck
