# FMT trash 模块
#
# 回收站的两级合成视图：文件级（file.json 权威）与桶级（trash/<用户>/.original 权威）。
# 本模块只做合并与标识解析，不自己存状态。

target_sources(fmt_core PRIVATE
    trash.cpp
)
