# ============================================================================
#  FMT - 顶层构建脚本
#
#  构建目录只有 cmake-build-debug 一个（预设 debug），与 CLion 默认配置一致：
#      cmake --preset debug
#      cmake --build --preset debug
#
#  产物：cmake-build-debug/bin/fmt.exe
#
#  目录说明：
#      include/fmt/   公共头文件
#      src/           各模块源码，源文件在各自 CMakeLists.txt 中登记
#      third_party/   vendored 单头文件库
#      tests/         单元测试
#
#  规范依据：docx/ 下的四份设计文档
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# 静态链接 MSVC 运行库，保证最终产物只有一个 fmt.exe
#
# 必须在 project() 之前设置：MSVC 变量由 project() 检测编译器后才定义，
# 在此之前恒为假，用 if(MSVC) 包起来会被静默跳过。该变量只被 MSVC 生成器
# 使用，其他编译器忽略，因此无条件设置是安全的。
# ---------------------------------------------------------------------------
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(FMT
        VERSION 1.0.0
        DESCRIPTION "FMT - Windows File Management System"
        LANGUAGES CXX
        HOMEPAGE_URL "https://github.com/sczerofox/FMT")

# ---------------------------------------------------------------------------
# C++ 标准
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 源码与执行字符集统一 UTF-8，保证中文字面量不触发 C4819。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Windows 头文件纪律：
#   WIN32_LEAN_AND_MEAN 阻止 windows.h 拉进 winsock v1，否则它与 cpp-httplib
#   使用的 winsock2.h 会撞成「struct 类型重定义」。
#   NOMINMAX 避免 min/max 宏污染标准库调用。
if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# 产物集中到 <build>/bin，便于定位 fmt.exe。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# 导出 compile_commands.json 供 clangd / IDE 解析头文件。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Debug 下不要 /RTC1
#
# MSVC 默认 Debug 标志含 /RTC1，它会在函数入口对栈变量做哨兵检查，与本项目
# 的 std::filesystem::path + std::mutex 组合互相干扰，表现为函数返回时误报
# 「栈变量损坏」并以访问违例终止进程，崩溃点与真正的问题无关。保留 /Zi /Od。
# ---------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Ob0 /Od")

# ---------------------------------------------------------------------------
# 编译警告
# ---------------------------------------------------------------------------
add_library(fmt_project_options INTERFACE)
if(MSVC)
    target_compile_options(fmt_project_options INTERFACE /W4 /permissive-)
else()
    target_compile_options(fmt_project_options INTERFACE
                           -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# ---------------------------------------------------------------------------
# 版本头（生成，请勿手工修改）
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/fmt/version.hpp"
    @ONLY)

# ---------------------------------------------------------------------------
# 第三方库（vendored 单头文件，构建不需要网络）
# ---------------------------------------------------------------------------
add_subdirectory(third_party)

# ---------------------------------------------------------------------------
# 目标
#
#   fmt_core  静态库：全部模块的实现。测试与可执行文件共用同一份代码，
#             因此模块通过 target_sources(fmt_core ...) 登记源文件。
#   fmt       可执行文件 fmt.exe：只包含入口点 src/main.cpp。
# ---------------------------------------------------------------------------
add_library(fmt_core STATIC)

target_include_directories(fmt_core
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_BINARY_DIR}/generated")

target_link_libraries(fmt_core PRIVATE fmt_project_options)

add_executable(fmt src/main.cpp)

# 应用程序清单：requestedExecutionLevel = asInvoker。
# 写成 requireAdministrator 会让双击每次都弹 UAC，按需提权也就没意义了。
target_sources(fmt PRIVATE "${CMAKE_SOURCE_DIR}/resources/fmt.manifest")

target_link_libraries(fmt PRIVATE fmt_core fmt_project_options)

# 各模块在这里登记源文件。
add_subdirectory(src)

include(CTest)                 # 定义 BUILD_TESTING，默认 ON
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

message(STATUS "FMT ${PROJECT_VERSION} | ${CMAKE_BUILD_TYPE} | C++${CMAKE_CXX_STANDARD} | tests=${BUILD_TESTING}")
