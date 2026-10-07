# 第三方库（vendored 单头文件）
#
# 直接放进仓库，构建时不需要访问网络。
#
# 约束：最终产物只能是单个 fmt.exe。这两个库都是 header-only，
# 编译进可执行文件，不产生 DLL，符合该约束。
#
# 引入新库时必须先确认同一约束：
#   单头文件 / 纯头文件  → 可以
#   需要额外 DLL 的库    → 不引入（除非确认无替代方案）
#
# 包含路径以本目录为根，因此源码中写：
#   #include <nlohmann/json.hpp>
#   #include <cpp-httplib/httplib.h>

# nlohmann/json：JSON 解析与序列化
add_library(fmt_nlohmann_json INTERFACE)
target_include_directories(fmt_nlohmann_json INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# cpp-httplib：HTTP 服务端与客户端（同时提供 server 与 client）
add_library(fmt_cpp_httplib INTERFACE)
target_include_directories(fmt_cpp_httplib INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
# Windows 下需要 Winsock
if(WIN32)
    target_link_libraries(fmt_cpp_httplib INTERFACE ws2_32)
endif()
