# FMT config 模块
#
# config.json / server.json 的加载、校验、保存。
# 公共头文件位于 include/fmt/config/。

target_sources(fmt_core PRIVATE
    config.cpp
)

target_link_libraries(fmt_core PRIVATE fmt_nlohmann_json)
