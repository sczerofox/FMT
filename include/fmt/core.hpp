#pragma once

#include <string>
#include <string_view>

// Public API of the FMT core library.
//
// This is intentionally minimal: it exists so the skeleton has one real symbol
// to compile, link and test. Replace or extend it once the specification in
// docx/ is written.
namespace fmt {

/// Human-readable version of the project, e.g. "0.1.0".
[[nodiscard]] std::string_view version_string();

/// One-line greeting identifying the running build, e.g. "FMT 0.1.0".
[[nodiscard]] std::string banner();

}  // namespace fmt
