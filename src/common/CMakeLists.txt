# FMT common 模块
#
# 公共类型与工具：Error / Result / Time / String / Logger。
# 不依赖任何业务模块，公共头文件位于 include/fmt/common/。

target_sources(fmt_core PRIVATE
    error.cpp
    time.cpp
    string.cpp
    logger.cpp
)
