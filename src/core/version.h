#pragma once

namespace ck {

// The single source of truth for the version string. The packaging script
// reads it from here, so bump it in one place.
extern const char* kVersion;

}  // namespace ck
