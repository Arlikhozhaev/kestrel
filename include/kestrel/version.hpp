#pragma once

// Overridden by CMake from project(VERSION). Defaults keep the headers
// buildable under a bare compiler invocation.
#ifndef KESTREL_VERSION_MAJOR
#define KESTREL_VERSION_MAJOR 0
#endif
#ifndef KESTREL_VERSION_MINOR
#define KESTREL_VERSION_MINOR 1
#endif
#ifndef KESTREL_VERSION_PATCH
#define KESTREL_VERSION_PATCH 0
#endif

namespace kestrel {

inline constexpr int kVersionMajor = KESTREL_VERSION_MAJOR;
inline constexpr int kVersionMinor = KESTREL_VERSION_MINOR;
inline constexpr int kVersionPatch = KESTREL_VERSION_PATCH;

}  // namespace kestrel
