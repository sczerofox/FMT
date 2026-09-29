#include <exception>
#include <iostream>
#include <string_view>

#include "fmt/core.hpp"

namespace {

constexpr std::string_view kUsage =
    "Usage: FMT [options]\n"
    "\n"
    "Options:\n"
    "  -h, --help     Show this message and exit\n"
    "  -v, --version  Show the version and exit\n";

}  // namespace

int main(int argc, char** argv) {
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "-h" || arg == "--help") {
                std::cout << kUsage;
                return 0;
            }
            if (arg == "-v" || arg == "--version") {
                std::cout << fmt::version_string() << '\n';
                return 0;
            }
            std::cerr << "unknown argument: " << arg << "\n\n" << kUsage;
            return 2;
        }

        std::cout << fmt::banner() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
