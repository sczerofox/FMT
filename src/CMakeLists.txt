# FMT 源码模块
#
# 每个模块在自己的 CMakeLists.txt 中通过 target_sources(fmt_core ...) 登记源文件。
# 新增模块只需三步：建 src/<模块>/ 与 include/fmt/<模块>/、在这里加
# add_subdirectory(<模块>)、在该模块登记源文件。顶层构建脚本不需要改动。
#
# 依赖方向：
#   CLI / HTTP  ->  service  ->  core  ->  storage  ->  文件系统
#   ipc 是 CLI 与服务之间的通道，两条入口汇入同一个 service 层。
#   common 不依赖任何业务模块。

# 公共类型与工具：无业务依赖，最先引入
add_subdirectory(common)

# 底层数据访问与配置
add_subdirectory(storage)
add_subdirectory(config)

# CLI 与服务之间的命令通道
add_subdirectory(ipc)

# Windows Service：SCM 四条命令与服务端运行体
add_subdirectory(service)

# 业务模块
add_subdirectory(bucket)
add_subdirectory(file)
add_subdirectory(trash)
add_subdirectory(share)
add_subdirectory(user)

# 命令行界面（两条入口中的 CLI 一侧）
add_subdirectory(cli)

# 浏览器入口：HTTP
add_subdirectory(server)

# 路径计算与应用生命周期
add_subdirectory(core)
