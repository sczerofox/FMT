# Unit tests. Add one test source file per module as the project grows.
include(FetchContent)

set(FMT_CATCH2_TAG v3.7.1 CACHE STRING "Catch2 git tag to fetch")

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        ${FMT_CATCH2_TAG}
    GIT_SHALLOW    TRUE
    SYSTEM
)
FetchContent_MakeAvailable(Catch2)

add_executable(fmt_tests
    core_test.cpp
)

target_link_libraries(fmt_tests
    PRIVATE
        fmt_core
        fmt_project_options
        Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)

catch_discover_tests(fmt_tests)
