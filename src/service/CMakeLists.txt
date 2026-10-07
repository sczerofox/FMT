# FMT service 模块
#
# Windows Service：SCM 四条命令（install / uninstall / start / stop）、
# ServiceMain 与控制处理、Recovery、服务自身状态文件，以及服务端运行体
# （监听命名管道、维护当前数据根）。
#
# 需要 advapi32（SCM 与安全描述符）。

target_sources(fmt_core PRIVATE
    service.cpp
    runtime.cpp
)

if(WIN32)
    target_link_libraries(fmt_core PRIVATE advapi32)
endif()
