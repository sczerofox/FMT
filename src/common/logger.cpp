#include "fmt/common/logger.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"
#include "fmt/core/path.hpp"

namespace fmt {
namespace {

std::string format_line(LogLevel level, std::string_view module, std::string_view message) {
    std::string line = local_timestamp();
    line += " [";
    line += level_name(level);
    line += "] [";
    line += module;
    line += "] ";
    line += message;
    line += '\n';
    return line;
}

void write_console(bool to_stderr, const std::string& text) {
    std::FILE* stream = to_stderr ? stderr : stdout;
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fflush(stream);
}

std::uintmax_t size_of(const std::filesystem::path& path) {
    std::error_code code;
    const std::uintmax_t size = std::filesystem::file_size(path, code);
    return code ? 0 : size;
}

// 活动日志：清理时只认这两个名字，别的文件（用户手放的、将来新增的）一律不动。
constexpr const char* kActiveLogNames[] = {"fmt.log", "error.log"};
constexpr const char* kBackupSuffix = ".1";

// 保留 path 最新的 keep 字节：写临时文件 -> 校验 -> 原子替换。
// 正在被另一个进程追加时替换会失败（共享冲突）——返回 false，调用方跳过它。
bool trim_tail(const std::filesystem::path& path, std::uintmax_t keep, std::uintmax_t* freed) {
    const std::uintmax_t size = size_of(path);
    if (size <= keep) {
        return false;
    }

    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(static_cast<std::streamoff>(size - keep));
    if (!in) {
        return false;
    }

    std::string tail;
    tail.resize(static_cast<std::size_t>(keep));
    in.read(tail.data(), static_cast<std::streamsize>(tail.size()));
    tail.resize(static_cast<std::size_t>(in.gcount()));
    in.close();

    // 从头保留的可能是半行：丢掉第一个（不完整的）换行之前的内容，
    // 免得日志里出现一截断掉的时间戳，看起来像文件坏了。
    const std::size_t first_newline = tail.find('\n');
    if (first_newline == std::string::npos) {
        return false;  // 一个换行都没有：不正常，别动它
    }
    std::string kept = tail.substr(first_newline + 1);

    std::string content = local_timestamp() + " [INFO] [Log] 日志已裁剪：保留最近 " +
                          std::to_string(kept.size()) + " 字节（原 " + std::to_string(size) +
                          " 字节），更早的记录已删除\n";
    content += kept;

    const std::filesystem::path temporary = path.string() + ".trim.tmp";
    {
        std::ofstream out(temporary, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }

    std::error_code code;
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;  // 另一个进程正握着这个文件
    }

    if (freed != nullptr) {
        *freed = (size > content.size()) ? size - content.size() : 0;
    }
    return true;
}

}  // namespace

std::string_view level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
    }
    return "INFO";
}

Result<LogCleanup> cleanup_logs(const std::filesystem::path& log_directory,
                                std::uintmax_t budget, bool dry_run) {
    LogCleanup report;
    report.dry_run = dry_run;

    std::error_code code;
    if (!std::filesystem::is_directory(log_directory, code)) {
        // 还没写过日志：没什么可清的，也不是错误。
        report.message = "日志目录不存在，无需清理";
        return report;
    }

    // 目录里有什么、多大：一次性扫出来，后面按名字取。
    struct Item {
        std::filesystem::path path;
        std::uintmax_t size = 0;
    };
    std::vector<Item> backups;
    std::vector<Item> actives;
    std::uintmax_t total = 0;

    for (const auto& entry : std::filesystem::directory_iterator(log_directory, code)) {
        if (code) {
            break;
        }
        std::error_code type_code;
        if (!entry.is_regular_file(type_code)) {
            continue;
        }
        const std::string name = path_to_utf8(entry.path().filename());
        Item item{entry.path(), size_of(entry.path())};
        total += item.size;

        bool is_backup = false;
        for (const char* active : kActiveLogNames) {
            if (name == std::string(active) + kBackupSuffix) {
                is_backup = true;
                break;
            }
        }
        if (is_backup) {
            backups.push_back(std::move(item));
            continue;
        }
        for (const char* active : kActiveLogNames) {
            if (name == active) {
                actives.push_back(std::move(item));
                break;
            }
        }
    }

    report.over_budget = (total > budget);
    if (!report.over_budget) {
        report.remaining_bytes = total;
        report.message = "日志占用 " + format_size(total) + "，未超过上限 " +
                         format_size(budget) + "，未做任何清理";
        return report;
    }

    std::uintmax_t projected = total;

    // ① 先删轮转备份（纯附加物）。删不掉（另一个进程握着）就跳过。
    for (const Item& item : backups) {
        if (!dry_run) {
            std::error_code remove_code;
            if (!std::filesystem::remove(item.path, remove_code) || remove_code) {
                report.notes.push_back(path_to_utf8(item.path.filename()) +
                                       "：正被另一个进程使用，本次跳过");
                continue;
            }
        }
        report.notes.push_back(path_to_utf8(item.path.filename()) + "：删除（释放 " +
                               format_size(item.size) + "）");
        projected -= std::min(projected, item.size);
        ++report.removed_backups;
        report.freed_bytes += item.size;
    }

    // ② 仍超预算：活动文件保留最新的尾部。预算是两个活动文件共享的，先各分一半，
    //    再按「谁大谁多留」调不平——这里保持简单：各分一半，且不低于下限。
    if (budget > 0 && projected > budget) {
        const std::uintmax_t share = std::max(budget / 2, kMinActiveLogBytes);
        for (const Item& item : actives) {
            if (item.size <= share) {
                continue;
            }
            std::uintmax_t freed = 0;
            if (!dry_run && !trim_tail(item.path, share, &freed)) {
                report.notes.push_back(path_to_utf8(item.path.filename()) +
                                       "：正被另一个进程使用，本次跳过");
                continue;
            }
            // dry_run 时按「保留 share + 一行说明」估个大概，不必精确。
            const std::uintmax_t estimate = dry_run ? item.size - share : freed;
            report.notes.push_back(path_to_utf8(item.path.filename()) + "：裁剪为最新 " +
                                   format_size(share) + "（释放 " + format_size(estimate) + "）");
            projected -= std::min(projected, estimate);
            ++report.trimmed_files;
            report.freed_bytes += estimate;
        }
    }

    // 真正剩下的占用**重新扫一遍**：`projected` 是估算（裁剪之后那行说明本身也占
    // 几个字节、被跳过的文件没变），报给用户的数字必须与磁盘对得上。
    if (dry_run) {
        report.remaining_bytes = projected;
    } else {
        std::uintmax_t after = 0;
        std::error_code rescan;
        for (const auto& entry : std::filesystem::directory_iterator(log_directory, rescan)) {
            if (rescan) {
                break;
            }
            std::error_code type_code;
            if (entry.is_regular_file(type_code)) {
                after += size_of(entry.path());
            }
        }
        report.remaining_bytes = after;
        report.freed_bytes = (total > after) ? total - after : 0;
    }

    report.message = std::string(dry_run ? "（预演）" : "") + "日志清理：释放 " +
                     format_size(report.freed_bytes) + "，剩余约 " +
                     format_size(report.remaining_bytes) + "（上限 " + format_size(budget) + "）";
    return report;
}

bool Logger::append_line(const std::filesystem::path& path, const std::string& line) {
    // 开 -> 写 -> 关：句柄不长期持有，另一个进程（CLI 与服务共用同一个日志）
    // 才可能在需要时把文件改名轮转。追加语义由 ios::app 保证（写到文件末尾）。
    std::ofstream stream(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }
    stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

void Logger::rotate_if_needed() {
    if (options_.max_log_bytes == 0) {
        return;  // 明确要求不轮转
    }
    if (++lines_since_check_ < kRotationCheckInterval) {
        return;
    }
    lines_since_check_ = 0;

    const auto rotate_one = [this](const std::filesystem::path& path) -> bool {
        if (size_of(path) <= options_.max_log_bytes) {
            return false;
        }
        std::filesystem::path previous = path;
        previous += L".1";
        std::error_code code;
        // 先删旧的一代（rename 在目标已存在时会失败），再改名。
        // 另一个进程正巧在写这个文件时改名会失败——那就下一次检查再试，不报错。
        std::filesystem::remove(previous, code);
        code.clear();
        std::filesystem::rename(path, previous, code);
        return !code;
    };

    if (rotate_one(all_path_)) {
        // 这一行写进**新**文件，说明刚刚发生了什么；用户翻日志时能看到断点。
        const std::string note = local_timestamp() + " [INFO] [Log] 日志超过 " +
                                 std::to_string(options_.max_log_bytes) +
                                 " 字节，已轮转：fmt.log -> fmt.log.1\n";
        append_line(all_path_, note);
    }
    rotate_one(error_path_);
}

Result<std::unique_ptr<Logger>> Logger::open(const std::filesystem::path& log_directory,
                                             Options options) {
    std::error_code code;
    std::filesystem::create_directories(log_directory, code);
    if (code && !std::filesystem::is_directory(log_directory)) {
        return make_error(ErrorCode::DirectoryCreateFailed,
                          "无法创建日志目录：" + to_utf8(log_directory.wstring()));
    }

    auto logger = std::unique_ptr<Logger>(new Logger());
    logger->options_ = options;
    logger->all_path_ = log_directory / "fmt.log";
    logger->error_path_ = log_directory / "error.log";
    logger->file_logging_ = true;

    // 仍然在打开时就验一次可写：日志写不了要立刻报出来，而不是等第一条日志静默丢掉。
    if (!append_line(logger->all_path_, std::string{})) {
        return make_error(ErrorCode::IoError,
                          "无法打开日志文件：" + to_utf8(logger->all_path_.wstring()));
    }
    if (!append_line(logger->error_path_, std::string{})) {
        return make_error(ErrorCode::IoError,
                          "无法打开日志文件：" + to_utf8(logger->error_path_.wstring()));
    }

    return std::unique_ptr<Logger>(std::move(logger));
}

std::unique_ptr<Logger> Logger::console_only(Options options) {
    auto logger = std::unique_ptr<Logger>(new Logger());
    logger->options_ = options;
    return logger;
}

std::unique_ptr<Logger> Logger::silent() {
    auto logger = std::unique_ptr<Logger>(new Logger());
    logger->silent_ = true;
    return logger;
}

Logger::~Logger() = default;

void Logger::log(LogLevel level, std::string_view module, std::string_view message) {
    if (silent_) {
        return;
    }

    const std::string line = format_line(level, module, message);
    const std::lock_guard<std::mutex> guard(mutex_);

    if (file_logging_) {
        rotate_if_needed();
        append_line(all_path_, line);
        // error.log **仅 ERROR 级**（开发文档 §65）。
        if (level == LogLevel::Error) {
            append_line(error_path_, line);
        }
    }

    const bool to_console =
        level == LogLevel::Info ? options_.info_to_console : options_.warn_error_to_console;
    if (to_console) {
        write_console(options_.console_to_stderr && level != LogLevel::Info, line);
    }
}

void Logger::info(std::string_view module, std::string_view message) {
    log(LogLevel::Info, module, message);
}

void Logger::warn(std::string_view module, std::string_view message) {
    log(LogLevel::Warn, module, message);
}

void Logger::error(std::string_view module, std::string_view message) {
    log(LogLevel::Error, module, message);
}

}  // namespace fmt
