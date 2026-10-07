# FMT file 模块
#
# 文件业务：上传（HTTP 下载 / 本地复制 -> temp/ -> repository/）、列表、查询、软删除。
# 下载用 cpp-httplib 的 Client（HTTPS 需要 OpenSSL，V1 不支持，见技术文档第 86/5124 行）。

target_sources(fmt_core PRIVATE
    file.cpp
)

if(WIN32)
    target_link_libraries(fmt_core PRIVATE fmt_cpp_httplib)
endif()
