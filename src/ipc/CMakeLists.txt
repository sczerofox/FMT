# FMT ipc 模块
#
# CLI 与服务之间的命名管道：帧协议、连接、DACL 与 MIC 安全描述符。
# 只有 Windows 有效，需要 advapi32（安全描述符）。

target_sources(fmt_core PRIVATE
    protocol.cpp
    pipe.cpp
)

target_link_libraries(fmt_core PUBLIC fmt_nlohmann_json)

if(WIN32)
    target_link_libraries(fmt_core PRIVATE advapi32)
endif()
