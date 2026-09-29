# fmt_core - the project's reusable library.
#
# Everything that is not the entry point belongs here so that tests can link
# against the same code the application uses.
add_library(fmt_core STATIC
    fmt.cpp
)

target_include_directories(fmt_core
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_BINARY_DIR}/generated"   # generated fmt/version.hpp
)

target_link_libraries(fmt_core PRIVATE fmt_project_options)

# Namespaced alias so consumers can link fmt::fmt_core if this is vendored.
add_library(fmt::fmt_core ALIAS fmt_core)

# ---- Install rules -------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS fmt_core
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(FILES "${PROJECT_BINARY_DIR}/generated/fmt/version.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/fmt")
