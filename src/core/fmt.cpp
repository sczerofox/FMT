#include "fmt/core.hpp"

#include "fmt/version.hpp"

namespace fmt {

std::string_view version_string() {
    return version::STRING;
}

std::string banner() {
    std::string text = "FMT ";
    text += version::STRING;
    return text;
}

}  // namespace fmt
