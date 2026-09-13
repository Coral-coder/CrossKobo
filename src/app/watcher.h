#pragma once
#include <string>
#include <vector>

namespace ck {

// Watches the drive for the stock Kobo software opening one of CrossKobo's
// trigger files, and launches CrossKobo when it does. This is how the
// add-on appears inside a closed application that cannot be extended:
// tapping "CrossKobo Catalogues" in the Kobo library opens the browser.
//
// Runs forever; started by its own init script, which does nothing else.
int run_watcher(const std::string& launcher);

// The arguments a trigger filename launches CrossKobo with, or "\xff" when
// the name is not a trigger. Exposed for tests.
std::string watcher_command_for(const std::string& filename);

// The filenames the watcher reacts to, which are also what the reader sees
// in the stock library.
std::vector<std::string> watcher_trigger_names();

// Creates any trigger file that is missing, without overwriting one the
// user has edited. Returns false if any could not be written.
bool write_trigger_files(const std::string& dir);

}  // namespace ck
