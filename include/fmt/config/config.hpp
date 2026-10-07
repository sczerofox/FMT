// 配置
//
// config/config.json 与 config/server.json 的加载、校验、保存。
//
// 规则（docx/FMT 开发文档.md §10、§11）：
//   文件不存在        -> 写默认配置（首次运行的正常路径）
//   文件存在但损坏    -> 报错并停止相关初始化，**绝不删除或重置原文件**
//   字段缺失或类型不对 -> 用默认值补齐并回写
//   保存              -> 写 .tmp -> 校验 -> 替换
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fmt/common/error.hpp"
#include "fmt/core/path_manager.hpp"

namespace fmt {

// config/config.json
struct Config {
    static constexpr int kVersion = 1;

    int version = kVersion;
    std::string current_user;
    std::string current_bucket;
    std::uint64_t max_upload_size = 52428800;  // 50 MB
    std::string size_unit = "MB";
    std::string language = "zh-CN";
};

// config/server.json
struct ServerConfig {
    static constexpr int kVersion = 1;

    int version = kVersion;
    bool enabled = false;  // 默认关闭；安装流程在启用网络服务时置为 true
    std::string host = "127.0.0.1";
    int port = 4122;
};

Result<Config> load_config(const PathManager& paths);
Status save_config(const PathManager& paths, const Config& config);

Result<ServerConfig> load_server_config(const PathManager& paths);
Status save_server_config(const PathManager& paths, const ServerConfig& config);

// 缺失时写出默认的 config.json / server.json（已存在一律不动）。
// 默认值只定义在本模块，数据根初始化只调用，不重复一份字段表。
// 写出的文件路径追加进 created（UTF-8、正斜杠），便于调用方报告。
Status ensure_default_config_files(const PathManager& paths, std::vector<std::string>* created);

}  // namespace fmt
