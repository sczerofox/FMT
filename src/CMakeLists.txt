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
