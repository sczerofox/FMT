#include "fmt/common/logger.hpp"

#include <cstdio>
#include <fstream>
#include <system_error>
#include <utility>

#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"

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
