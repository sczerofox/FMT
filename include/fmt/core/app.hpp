// 应用上下文与初始化
//
// 上下文对象（不是全局单例）：测试可以按自己的临时目录构造独立上下文。
//
//   AppContext
//    ├── PathManager   数据根
//    ├── Logger        日志（CLI 与 Service 共写同一个文件）
//    ├── Config        config.json
//    └── ServerConfig  server.json
//
// 数据根的初始化与检查对 **Service 与 CLI 都开放**，两边看到的是同一套规则：
// 只补缺失的目录与文件，已存在的一律不动，损坏的 JSON 只报告、绝不重置
// （docx/FMT 技术文档.md §4.4）。
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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

// 数据根体检结果。只报告，不改动任何东西。
struct RootReport {
    std::vector<std::string> missing_directories;  // 目录名
    std::vector<std::string> missing_files;        // UTF-8 路径
    // 文件在但不可用：读不出来、JSON 非法、版本不受支持。
    // 这些**不会被自动修复**——按冻结规则，损坏的 JSON 不静默重置。
    std::vector<std::string> broken_files;         // 「路径（FMT-006 消息）」

    bool complete() const {
        return missing_directories.empty() && missing_files.empty() && broken_files.empty();
    }
};

// 检查六个目录、六个默认 JSON 是否齐全，已有的 JSON 能否读出且版本受支持。
RootReport check_root(const PathManager& paths);

// 补建缺失的目录与默认 JSON（幂等：只补不缺，绝不删除或覆盖已有内容）。
struct RootRepair {
    std::vector<std::string> created_directories;
    std::vector<std::string> created_files;
};

Result<RootRepair> ensure_root(const PathManager& paths);

// 建目录 + 写默认 JSON，返回 PathManager。等价于 ensure_root + 重读配置。
Result<std::unique_ptr<PathManager>> initialize_root(const std::filesystem::path& root);

// 服务侧的完整初始化：initialize_root + 打开日志 + 加载配置。
Result<std::unique_ptr<AppContext>> initialize_service_context(const std::filesystem::path& root);

}  // namespace fmt
