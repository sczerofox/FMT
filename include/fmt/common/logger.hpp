// 日志
//
// 去向规则（docx/FMT 开发文档.md §65）：
//
//   级别    fmt.log   error.log   控制台
//   INFO     是         否        视场景
//   WARN     是         是        是
//   ERROR    是         是        是
//
// V1 不设 DEBUG，也不做异步日志、日志队列、压缩或轮转。
// **只有 Service 打开日志文件**；CLI 用 console_only()，避免两个进程争抢同一文件。
#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "fmt/common/error.hpp"

namespace fmt {

enum class LogLevel { Info, Warn, Error };

std::string_view level_name(LogLevel level);

// 每行格式：时间 [级别] [模块] 消息
//     2026-10-05 23:40:01 [INFO] [Config] 加载 config.json
class Logger {
public:
    struct Options {
        // INFO 是否也打印到控制台（交互式 CLI 打开，后台服务关掉）。
        bool info_to_console = true;
        // WARN / ERROR 是否打印到控制台。
        bool warn_error_to_console = true;
        // 控制台输出走 stderr（提示符场景下避免与结果混在一起）。
        bool console_to_stderr = false;
    };

    // 打开 <log_directory>/fmt.log 与 error.log；目录不存在时尝试创建。
    static Result<std::unique_ptr<Logger>> open(const std::filesystem::path& log_directory,
                                                Options options = {});
    // 只输出控制台，不写文件（CLI 使用）。
    static std::unique_ptr<Logger> console_only(Options options = {});
    // 什么都不做（初始化失败时的兜底，保证调用方不必到处判空）。
    static std::unique_ptr<Logger> silent();

    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, std::string_view module, std::string_view message);
    void info(std::string_view module, std::string_view message);
    void warn(std::string_view module, std::string_view message);
    void error(std::string_view module, std::string_view message);

private:
    Logger() = default;

    std::mutex mutex_;
    std::ofstream all_log_;
    std::ofstream error_log_;
    Options options_{};
    bool silent_ = false;
};

}  // namespace fmt
