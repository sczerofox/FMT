// 应用上下文与初始化
//
// 上下文对象（不是全局单例）：测试可以按自己的临时目录构造独立上下文。
//
//   AppContext
//    ├── PathManager   数据根
//    ├── Logger        日志（只有 Service 打开文件）
//    ├── Config        config.json
//    └── ServerConfig  server.json
//
// 初始化的执行者是 **Service**：建目录、补默认 JSON、打开日志。CLI 只读，
// 不创建任何东西（docx/FMT 重构设计.md §5.2）。
#pragma once

#include <filesystem>
#include <memory>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/path_manager.hpp"

namespace fmt {

struct AppContext {
    std::unique_ptr<PathManager> paths;
    std::unique_ptr<Logger> logger;
    Config config;
    ServerConfig server_config;
};

// 建目录 + 补默认 JSON，幂等：已存在的文件一律保持原样，绝不删除或覆盖。
// 服务在首次启动、以及每次切换到新数据根时调用。
Result<std::unique_ptr<PathManager>> initialize_root(const std::filesystem::path& root);

// 服务侧的完整初始化：initialize_root + 打开日志 + 加载配置。
Result<std::unique_ptr<AppContext>> initialize_service_context(const std::filesystem::path& root);

}  // namespace fmt
