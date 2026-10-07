# FMT core 模块
#
# 路径计算与应用生命周期。业务对象不在这里。

target_sources(fmt_core PRIVATE
    path.cpp
    path_manager.cpp
    app.cpp
)
