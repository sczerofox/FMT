#include "fmt/common/logger.hpp"

#include <cstdio>
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

    const std::filesystem::path all_path = log_directory / "fmt.log";
    const std::filesystem::path error_path = log_directory / "error.log";

    logger->all_log_.open(all_path, std::ios::out | std::ios::app | std::ios::binary);
    if (!logger->all_log_.is_open()) {
        return make_error(ErrorCode::IoError, "无法打开日志文件：" + to_utf8(all_path.wstring()));
    }

    logger->error_log_.open(error_path, std::ios::out | std::ios::app | std::ios::binary);
    if (!logger->error_log_.is_open()) {
        return make_error(ErrorCode::IoError, "无法打开日志文件：" + to_utf8(error_path.wstring()));
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

    if (all_log_.is_open()) {
        all_log_ << line;
        all_log_.flush();
    }

    if (level == LogLevel::Error && error_log_.is_open()) {
        error_log_ << line;
        error_log_.flush();
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
