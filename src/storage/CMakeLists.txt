# FMT storage 模块
#
# 底层数据访问：文本与 JSON 的原子读写、目录创建、文件检查。
# 不负责业务规则（文件能不能删由业务模块决定）。
#
# JSON 使用 vendored 的 nlohmann/json，接口目标定义在 third_party/CMakeLists.txt。

target_sources(fmt_core PRIVATE
    storage.cpp
)

# storage 的公共头文件里出现了 nlohmann::json，因此依赖必须是 PUBLIC：
# 任何包含 storage.hpp 的目标都要能看到这个头文件。
target_link_libraries(fmt_core PUBLIC fmt_nlohmann_json)
