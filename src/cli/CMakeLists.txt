# FMT cli 模块
#
# 命令行界面：横幅、fmt> 提示符、service 四条命令的提权、业务命令经命名管道。
# CLI 只读：不建目录、不写 JSON、不写日志文件。

target_sources(fmt_core PRIVATE
    cli.cpp
    elevation.cpp
)

if(WIN32)
    # ShellExecuteExW（runas 提权）在 shell32
    target_link_libraries(fmt_core PRIVATE shell32)
endif()
