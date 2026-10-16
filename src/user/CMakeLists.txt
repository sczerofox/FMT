# FMT user 模块
#
# 账号与访问令牌（data/user.json）：HTTP 接口的认证来源。
# 口令只存 PBKDF2-SHA256 摘要；盐由用户名派生（见 user.hpp 的 directory_salt），
# 所以「删除用户后按同样的用户名重建」会得到完全一样的文件——这是
# config.json 的 uuid（密码只能改一次）判定所依赖的性质。
#
# user_service.cpp 是 CLI 与 HTTP 共用的领域入口（create/update/delete/get）。

target_sources(fmt_core PRIVATE
    user.cpp
    user_service.cpp
)

