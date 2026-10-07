# FMT file 模块
#
# 文件业务：上传（HTTP(S) 下载 / 本地复制 -> temp/ -> repository/）、列表、查询、软删除。
# 下载走 common/http_client（WinHTTP + Schannel，https 不需要 OpenSSL），
# 服务端的浏览器入口才用 cpp-httplib。

target_sources(fmt_core PRIVATE
    file.cpp
)
