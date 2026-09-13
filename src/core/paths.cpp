#include "core/paths.h"

namespace ck {

namespace {
Paths g_paths;
}

Paths& paths() { return g_paths; }
void set_paths(const Paths& p) { g_paths = p; }

}  // namespace ck
