#pragma once
#include <string>

#include "catalogues/httpd.h"

namespace catalogues {

// The catalogue service: every page the Kobo's browser asks for. Catalogues
// come from a text file on the drive, re-read on every request so an edit
// over USB shows up on the next tap. Feeds and searches go through the OPDS
// and search-format clients; downloads land in the Downloads folder on the
// drive, where the Kobo's own library finds them.
//
// Pages:
//   GET  /                    the catalogue list (?open=Name jumps to one)
//   GET  /c/N                 a catalogue's root feed, or ?url= a feed in it
//   GET  /c/N/search?q=       a search, in whichever format the catalogue speaks
//   GET  /c/N/book?...        one book, with a download button
//   POST /c/N/download        downloads it and says where it went
//   GET  /downloads           what has been downloaded
//   GET  /help                how the catalogue file works
//   GET  /status              a line of JSON, for the launcher's ping
Response handle(const Request& request);

// Where downloads go, under the drive root.
std::string download_dir();

// Writes the catalogue file with its explanation and the public catalogues
// in it, if there is none yet.
void ensure_catalogue_file();

// The text that file starts out with.
std::string catalogue_file_template();

}  // namespace catalogues
