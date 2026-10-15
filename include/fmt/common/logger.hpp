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
//
// **清理**：轮转管「单个文件别太大」，清理管「整个 log/ 别太大」。
// 两份日志 + 两份 .1 备份最多约 20 MB，而且**每个数据根各一份**；所以启动时
// （CLI 与服务）与 `log clean` 都会调 cleanup_logs()：先删 .1 备份，仍超预算就把
// 活动文件裁成「最新的一段」，不超预算时一个字节都不动。见下。
//
// V1 仍然不设 DEBUG，也不做异步日志、日志队列或压缩。
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/common/error.hpp"

namespace fmt {

enum class LogLevel { Info, Warn, Error };

std::string_view level_name(LogLevel level);

// 单个日志文件的默认上限；超过就轮转成 <名字>.1。
inline constexpr std::uintmax_t kDefaultMaxLogBytes = 5 * 1024 * 1024;

// 整个 log/ 目录的默认预算（`fmt.log` + `error.log` + 两份 .1 备份）。
// 8 MB：轮转本身已经能挡住「一个文件无限长大」，这一层只是把「两代 × 两个文件」
// 的约 20 MB 上限压到 8 MB，并且让用户有一条随时能收一次的路（`log clean`）。
inline constexpr std::uintmax_t kDefaultLogBudgetBytes = 8 * 1024 * 1024;

// 清理之后单个活动文件至少保留这么多：预算再小，也不该把日志裁到一个字节不剩
// （出事时那几行往往就是全部线索）。
inline constexpr std::uintmax_t kMinActiveLogBytes = 256 * 1024;

// 清理一次的结果。`dry_run` 为真时统计的是**如果真清会怎样**，磁盘一个字节都没动。
struct LogCleanup {
    bool dry_run = false;
    bool over_budget = false;            // 清理前是否超过预算（false = 什么都没做）
    std::size_t removed_backups = 0;     // .1 备份被删掉的数量
    std::size_t trimmed_files = 0;       // 被裁成「最新一段」的活动文件数
    std::uintmax_t freed_bytes = 0;      // 释放（或将释放）的字节数
    std::uintmax_t remaining_bytes = 0;  // 之后 log/ 里的总占用
    std::vector<std::string> notes;      // 明细：「fmt.log：5.0MB -> 512KB」…
    std::string message;                 // 给用户看的一句话
};

// 清理 <log_directory> 下的日志。三步：
//   ① 删掉轮转备份（fmt.log.1 / error.log.1）——纯附加物，删了不丢任何新信息；
//   ② 仍超预算：活动文件只保留**最新的尾部**一段（至少 kMinActiveLogBytes），
//      并在顶部补一行说明，断点看得见；
//   ③ **不超预算时一个字节都不动**（不删、不裁、不重写）。
// 目录不存在 → 什么都不做（不是错误）。文件被另一个进程握着 → 跳过它、不报错，
// 下一次（下一个 CLI / 服务启动）再来。`budget == 0` 表示只删备份、不裁活动文件。
Result<LogCleanup> cleanup_logs(const std::filesystem::path& log_directory,
                                std::uintmax_t budget = kDefaultLogBudgetBytes,
                                bool dry_run = false);

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
