# fmt_app - application entry point.
add_executable(fmt_app
    main.cpp
)

target_link_libraries(fmt_app
    PRIVATE
        fmt_core
        fmt_project_options
)

# Allow `cmake --build . --target FMT` to keep working: CLion and older
# instructions may refer to the target by the project name.
set_target_properties(fmt_app PROPERTIES OUTPUT_NAME "FMT")
