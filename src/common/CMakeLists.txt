# FMT common 模块
#
# 公共类型与工具：Error / Result / Time / String / Logger / Hash / HTTP 客户端。
# 不依赖任何业务模块，公共头文件位于 include/fmt/common/。

target_sources(fmt_core PRIVATE
    error.cpp
    time.cpp
    string.cpp
    logger.cpp
    envelope.cpp
    validation.cpp
    hash.cpp
    http_client.cpp
)

if(WIN32)
    # MD5 走 Windows CNG（bcrypt.dll）
    target_link_libraries(fmt_core PRIVATE bcrypt)
    # 下载走 WinHTTP + Schannel：系统组件，不用分发 OpenSSL 的 DLL
    target_link_libraries(fmt_core PRIVATE winhttp)
endif()
