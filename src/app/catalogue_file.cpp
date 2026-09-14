#include "app/catalogue_file.h"

#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/str.h"
#include "net/http.h"   // Url
#include "net/libgen.h"

namespace ck {
namespace {

// Works out which client a line needs from the address itself, so a file
// can mix a friend's OPDS server with a search-format one and neither has
// to say which it is.
void infer_format(Settings::Catalogue& c) {
  const std::string& address = c.url.empty() ? c.search : c.url;
  if (looks_like_libgen_endpoint(address)) {
    c.format = "libgen";
    if (c.url.empty()) {
      c.url = c.search;
      c.search.clear();
    }
    return;
  }
  bool has_placeholder = address.find("{searchTerms}") != std::string::npos ||
                         address.find("{searchterms}") != std::string::npos ||
                         address.find("{query}") != std::string::npos;
  if (has_placeholder && c.search.empty()) {
    c.search = c.url;
    c.url.clear();
  }
}

Settings::Catalogue from_line(const std::string& line) {
  Settings::Catalogue c;
  std::vector<std::string> fields = split(line, '|');
  std::vector<std::string> values;
  for (const std::string& field : fields) values.push_back(trim(field));

  // "address" on its own, or "name | address | user | password". The field
  // holding "://" is the address wherever it sits, which makes a one-field
  // line and a named line the same code path.
  size_t address_at = values.size();
  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i].find("://") != std::string::npos) {
      address_at = i;
      break;
    }
  }
  if (address_at == values.size()) return c;   // no address: not a catalogue
  c.url = values[address_at];
  if (address_at > 0) c.name = values[address_at - 1];
  // Anything after the address: a format word, then credentials.
  std::vector<std::string> rest(values.begin() + (long)address_at + 1, values.end());
  for (const std::string& value : rest) {
    std::string lower = to_lower(value);
    if (lower == "libgen" || lower == "opds") {
      c.format = lower;
    } else if (c.user.empty()) {
      c.user = value;
    } else if (c.password.empty()) {
      c.password = value;
    }
  }
  if (c.name.empty()) {
    Url parsed;
    c.name = Url::parse(c.url, parsed) ? parsed.host : c.url;
  }
  infer_format(c);
  c.from_file = true;
  return c;
}

}  // namespace

std::string catalogue_file_path() {
  std::string json = paths().data + "/catalogues.json";
  if (fs::exists(json)) return json;
  return paths().data + "/catalogues.txt";
}

std::vector<Settings::Catalogue> parse_catalogue_list(const std::string& text) {
  std::vector<Settings::Catalogue> out;
  std::string body = trim(text);
  if (body.empty()) return out;

  if (body[0] == '[' || body[0] == '{') {
    Json json;
    if (!Json::parse(body, json)) {
      CK_LOGW("catalogues: the file is not valid JSON, ignoring it");
      return out;
    }
    const Json* array = &json;
    if (json.is_object()) {
      if (const Json* found = json.find("catalogues")) array = found;
    }
    if (!array->is_array()) return out;
    for (const Json& item : array->items()) {
      if (!item.is_object()) continue;
      Settings::Catalogue c;
      c.name = item.get_string("name");
      c.url = item.get_string("url");
      c.search = item.get_string("search");
      c.format = item.get_string("format", "opds");
      c.user = item.get_string("user");
      c.password = item.get_string("password");
      if (c.url.empty() && c.search.empty()) continue;
      if (c.name.empty()) {
        Url parsed;
        std::string address = c.url.empty() ? c.search : c.url;
        c.name = Url::parse(address, parsed) ? parsed.host : address;
      }
      infer_format(c);
      c.from_file = true;
      out.push_back(c);
    }
    return out;
  }

  for (const std::string& raw : split(body, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;
    Settings::Catalogue c = from_line(line);
    if (c.url.empty() && c.search.empty()) {
      CK_LOGW("catalogues: skipping a line with no address: %s", line.c_str());
      continue;
    }
    out.push_back(c);
  }
  return out;
}

std::vector<Settings::Catalogue> load_catalogue_file() {
  std::string path = catalogue_file_path();
  std::string text;
  if (!fs::read_file(path, text)) return {};
  std::vector<Settings::Catalogue> list = parse_catalogue_list(text);
  CK_LOGI("catalogues: %zu from %s", list.size(), path.c_str());
  return list;
}

bool write_catalogue_file_template() {
  std::string path = paths().data + "/catalogues.txt";
  if (fs::exists(path) || fs::exists(paths().data + "/catalogues.json")) return false;
  const char* kTemplate =
      "# CrossKobo catalogues\n"
      "#\n"
      "# One server per line. Everything here shows up in the catalogue\n"
      "# browser alongside the ones you add on the device, and this file is\n"
      "# the only place these are defined - delete a line and it is gone.\n"
      "#\n"
      "# Use addresses rather than IP numbers where you can: the same line\n"
      "# then works at home and away, as long as the name resolves from\n"
      "# wherever the Kobo is. An https address needs curl or wget on the\n"
      "# device; http works either way.\n"
      "#\n"
      "#   Name | address\n"
      "#   Name | address | user | password\n"
      "#   Name | address | libgen\n"
      "#\n"
      "# The format is worked out from the address: one naming search.php or\n"
      "# json.php is searched in that format, one with {searchTerms} in it is\n"
      "# a search endpoint, and anything else is read as an OPDS feed. Say\n"
      "# \"libgen\" or \"opds\" outright if a guess comes out wrong.\n"
      "#\n"
      "# Examples, commented out - a friend can send you a line to paste:\n"
      "#\n"
      "# A friend's library | https://books.example.net/opds | reader | secret\n"
      "# A friend's server  | https://fic.example.net/search.php?req={searchTerms}\n"
      "\n";
  if (!fs::write_file_atomic(path, kTemplate)) {
    CK_LOGW("catalogues: could not write %s", path.c_str());
    return false;
  }
  CK_LOGI("catalogues: wrote a template to %s", path.c_str());
  return true;
}

}  // namespace ck
