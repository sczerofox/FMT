# FMT server 模块
#
# 浏览器入口：HTTP 监听（cpp-httplib，默认 127.0.0.1:4122）。
# CLI 不走这里，两条入口共用 common/envelope 的响应信封。

target_sources(fmt_core PRIVATE
    server.cpp
)

target_link_libraries(fmt_core PRIVATE fmt_cpp_httplib)
