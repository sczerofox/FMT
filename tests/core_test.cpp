#include <catch2/catch_test_macros.hpp>

#include <string>

#include "fmt/core.hpp"
#include "fmt/version.hpp"

TEST_CASE("version_string matches the generated version header", "[core]") {
    REQUIRE(fmt::version_string() == fmt::version::STRING);
    REQUIRE_FALSE(fmt::version_string().empty());
}

TEST_CASE("version components are numeric", "[core]") {
    const auto numeric = [](std::string_view text) {
        REQUIRE_FALSE(text.empty());
        for (const char c : text) {
            REQUIRE(c >= '0');
            REQUIRE(c <= '9');
        }
    };

    numeric(fmt::version::MAJOR);
    numeric(fmt::version::MINOR);
    numeric(fmt::version::PATCH);
}

TEST_CASE("banner contains the version", "[core]") {
    const std::string text = fmt::banner();
    REQUIRE(text.starts_with("FMT "));
    REQUIRE(text.find(std::string{fmt::version_string()}) != std::string::npos);
}
