# FMT bucket 模块
#
# Bucket 业务规则（create / list / get / use / delete）。
# Bucket 就是 repository/<user>/<bucket>/ 这个目录，没有独立 ID。

target_sources(fmt_core PRIVATE
    bucket.cpp
)
