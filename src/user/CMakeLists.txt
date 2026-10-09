# FMT user 模块
#
# 账号与访问令牌（data/user.json）：HTTP 接口的认证来源。
# 密码只存 PBKDF2-SHA256 哈希 + 每用户随机盐；token 默认永久有效。

target_sources(fmt_core PRIVATE
    user.cpp
)
