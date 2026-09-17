#pragma once
#include <string>
#include <vector>

namespace ck {

// Watches the drive for the stock Kobo software opening a trigger file, and
// runs a launcher when it does. This is how an application appears inside a
// closed one that cannot be extended: a plainly named file in the Kobo
// library, which the reader taps like a book.
//
// Each application supplies its own triggers, so nothing here is specific
// to one of them.
struct Trigger {
  std::string filename;   // what the reader sees in the library
  std::string args;       // passed to the launcher
  std::string blurb;      // the file's own contents, explaining itself
};

// Runs forever; started by an init script that does nothing else.
int run_watcher(const std::string& launcher, const std::vector<Trigger>& triggers);

// The arguments a filename launches with, or "\xff" when it is not one of
// these triggers. Exposed for tests.
std::string watcher_command_for(const std::vector<Trigger>& triggers,
                                const std::string& filename);

// Creates any trigger file that is missing, without overwriting one the
// reader has edited. Returns false if any could not be written.
bool write_trigger_files(const std::string& dir, const std::vector<Trigger>& triggers);

}  // namespace ck
