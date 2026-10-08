// 日志
//
// 去向规则（docx/FMT 开发文档.md §65）：
//
//   级别    fmt.log   error.log   控制台
//   INFO     是         否        视场景
//   WARN     是         否        是
//   ERROR    是         是        是
//
// `error.log` **仅 ERROR 级**（§65 冻结的口径；本注释原来错写成「WARN 也进」）。
//
// **CLI 与 Service 都往同一个 <数据根>/log/fmt.log 追加**（每行一次写入，写完即关）：
// 两个进程共用一个文件，日志才跟着用户敲的命令走，而不是只在服务侧。
// 每行开-写-关是刻意的——文件句柄不长期持有，另一个进程才可能在需要时把它改名轮转。
//
// 轮转：`fmt.log` 超过 max_log_bytes（默认 5 MB）时改名为 `fmt.log.1`（只留一代），
// `error.log` 同理。改名失败（另一个进程正巧在写）就跳过，下一次检查再试。
// V1 仍然不设 DEBUG，也不做异步日志、日志队列或压缩。
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "fmt/common/error.hpp"

namespace fmt {

enum class LogLevel { Info, Warn, Error };

std::string_view level_name(LogLevel level);

// 单个日志文件的默认上限；超过就轮转成 <名字>.1。
inline constexpr std::uintmax_t kDefaultMaxLogBytes = 5 * 1024 * 1024;

// 每写多少行检查一次大小。不用时间做节流：写入频率差异很大，
// 按行计数既便宜又确定（测试也能预期到第几次检查会轮转）。
inline constexpr int kRotationCheckInterval = 64;

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
        // 单个日志文件的上限；0 表示不轮转（测试与需要完整日志的场景）。
        std::uintmax_t max_log_bytes = kDefaultMaxLogBytes;
    };

    // 打开 <log_directory>/fmt.log 与 error.log；目录不存在时尝试创建。
    static Result<std::unique_ptr<Logger>> open(const std::filesystem::path& log_directory,
                                                Options options = {});
    // 只输出控制台，不写文件。
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

    // 追加一行：开 -> 写 -> 关。返回是否成功。
    static bool append_line(const std::filesystem::path& path, const std::string& line);

    // 到了检查点且超过上限时，把 <path> 改名成 <path>.1（覆盖旧的一代）。
    // 调用时必须持有 mutex_。
    void rotate_if_needed();

    std::mutex mutex_;
    std::filesystem::path all_path_;
    std::filesystem::path error_path_;
    int lines_since_check_ = 0;
    Options options_{};
    bool silent_ = false;
    bool file_logging_ = false;
};

}  // namespace fmt
