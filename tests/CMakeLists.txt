# FMT 单元测试
#
# 每个模块一个 *_test.cpp，链接 fmt_core，测试与可执行文件共用同一份实现。
# 新增测试文件时在这里登记；fmt_test.cpp 提供运行器（无第三方依赖，可离线构建）。
add_executable(fmt_tests
    fmt_test.cpp
    error_test.cpp
    time_test.cpp
    string_test.cpp
    logger_test.cpp
    validation_test.cpp
    path_manager_test.cpp
    storage_test.cpp
    config_test.cpp
    app_test.cpp
    bucket_test.cpp
    ipc_test.cpp
    service_test.cpp
    cli_test.cpp
    server_test.cpp
)

target_link_libraries(fmt_tests
    PRIVATE
        fmt_core
        fmt_project_options
        fmt_cpp_httplib   # server 测试要用 HTTP 客户端打自己的接口
)

target_include_directories(fmt_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME fmt_tests COMMAND fmt_tests)
