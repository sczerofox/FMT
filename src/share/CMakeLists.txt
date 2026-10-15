# FMT share 模块
#
# 分享链接的数据面：share.json 的 create / get / list / delete，
# 以及下载前记账（register_download：管道的 `share download` 与 HTTP 的公开下载端点
# GET /api/share/<share_id>/download 都走它）。
# 中心约束是「Share 不得绕过 File 状态」（开发文档 §50）。

target_sources(fmt_core PRIVATE
    share.cpp
)
