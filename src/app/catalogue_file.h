#pragma once
#include <string>
#include <vector>

#include "app/settings.h"

namespace ck {

// Catalogues read from a file on the drive rather than added on the device.
//
// This is how a list of servers travels: edit it over USB, or paste in a
// line a friend sent you, and every one of them is in the browser. They are
// addresses, not IP numbers, so the same file works at home and away - as
// long as the name resolves from wherever the Kobo is.
//
// File-sourced entries are never written back into settings.json: the file
// stays the single place they are defined, so editing it out really removes
// them.

// Where the list lives: .crosskobo/catalogues.txt, with .json accepted too.
std::string catalogue_file_path();

// Parses either form. A JSON array of objects matches the shape settings
// uses; otherwise it is read as one catalogue per line:
//
//   Name | https://host/opds
//   Friend's server | https://fic.example.net/search.php?req={searchTerms}
//   Private | https://host/opds | user | password
//
// Blank lines and lines starting with # are ignored, and a line with no
// name takes the address as its name. Anything unparseable is skipped
// rather than failing the file, because one bad line should not cost the
// other twenty.
std::vector<Settings::Catalogue> parse_catalogue_list(const std::string& text);

// Reads the file, or returns an empty list when there is none.
std::vector<Settings::Catalogue> load_catalogue_file();

// Writes a commented template, if no file exists yet, so there is something
// to edit and something to paste a friend's line into.
bool write_catalogue_file_template();

}  // namespace ck
