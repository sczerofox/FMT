# ============================================================================
#  FMT - top-level build script
#
#  Layout
#    include/fmt/   public headers (installed / consumed by other targets)
#    src/core/      implementation of the fmt_core library
#    src/app/       application entry point (fmt_app executable)
#    tests/         unit tests (Catch2, registered with CTest)
#    tools/         developer scripts
#    resources/     runtime data files
#    docx/          design notes and specifications
# ============================================================================
cmake_minimum_required(VERSION 3.20)

project(FMT
        VERSION 0.1.0
        DESCRIPTION "FMT project"
        LANGUAGES CXX
        HOMEPAGE_URL "https://github.com/sczerofox/FMT")

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put the executable next to the libraries in a single output directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Export compile_commands.json so clangd / IDEs can resolve headers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a Debug build for single-config generators (Makefiles, Ninja);
# multi-config generators (Visual Studio, Xcode) pick this at build time.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

# Project-wide compiler warnings, inherited by every target that links fmt_core.
add_library(fmt_project_options INTERFACE)
if(MSVC)
    target_compile_options(fmt_project_options INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(fmt_project_options INTERFACE
                           -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# ---------------------------------------------------------------------------
# Version header (generated, do not edit by hand)
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/fmt/version.hpp"
    @ONLY)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
include(CTest)                 # defines BUILD_TESTING (default ON)

add_subdirectory(src/core)     # library: fmt_core
add_subdirectory(src/app)      # executable: fmt_app

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "FMT ${PROJECT_VERSION} | ${CMAKE_BUILD_TYPE} | C++${CMAKE_CXX_STANDARD} | tests=${BUILD_TESTING}")
