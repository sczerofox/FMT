# FMT share 模块
#
# 分享链接的数据面：share.json 的 create / get / list / delete，
# 以及下载前记账（register_download，供将来的 HTTP 下载端点调用）。
# 中心约束是「Share 不得绕过 File 状态」（开发文档 §50）。

target_sources(fmt_core PRIVATE
    share.cpp
)
