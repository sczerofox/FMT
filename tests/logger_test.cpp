// common/logger 的单元测试
#include "fmt/common/logger.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "fmt/storage/storage.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

namespace {

fmt::Logger::Options quiet_options() {
    fmt::Logger::Options options;
    options.info_to_console = false;
    options.warn_error_to_console = false;
    return options;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::istringstream stream(read_text(path));
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

}  // namespace

FMT_TEST(Logger, 级别名称) {
    FMT_CHECK_EQ(std::string(fmt::level_name(fmt::LogLevel::Info)), std::string("INFO"));
    FMT_CHECK_EQ(std::string(fmt::level_name(fmt::LogLevel::Warn)), std::string("WARN"));
    FMT_CHECK_EQ(std::string(fmt::level_name(fmt::LogLevel::Error)), std::string("ERROR"));
}

FMT_TEST(Logger, 写入两个文件且ERROR单独成文件) {
    fmt_test::TempDir temp("logger");
    const auto log_dir = temp / "log";

    {
        auto opened = fmt::Logger::open(log_dir, quiet_options());
        FMT_CHECK(fmt::ok(opened));
        std::unique_ptr<fmt::Logger> logger = std::move(std::get<std::unique_ptr<fmt::Logger>>(opened));

        logger->info("Config", "加载 config.json");
        logger->warn("Config", "配置文件不存在，使用默认配置");
        logger->error("File", "文件删除失败: example.txt");
    }

    const std::vector<std::string> all = read_lines(log_dir / "fmt.log");
    FMT_CHECK_EQ(all.size(), std::size_t{3});

    const std::vector<std::string> errors = read_lines(log_dir / "error.log");
    FMT_CHECK_EQ(errors.size(), std::size_t{1});

    // 格式：时间 [级别] [模块] 消息
    const std::string& line = all[1];
    FMT_CHECK_EQ(line.size() > 24, true);
    FMT_CHECK_EQ(line.substr(19, 8), std::string(" [WARN] "));
    FMT_CHECK_EQ(line.substr(27), std::string("[Config] 配置文件不存在，使用默认配置"));

    FMT_CHECK_EQ(errors[0].find("[ERROR] [File] 文件删除失败: example.txt") != std::string::npos,
                 true);
}

FMT_TEST(Logger, 追加而不是覆盖) {
    fmt_test::TempDir temp("logger-append");
    const auto log_dir = temp / "log";

    for (int i = 0; i < 2; ++i) {
        auto opened = fmt::Logger::open(log_dir, quiet_options());
        FMT_CHECK(fmt::ok(opened));
        std::unique_ptr<fmt::Logger> logger = std::move(std::get<std::unique_ptr<fmt::Logger>>(opened));
        logger->info("Main", "第 " + std::to_string(i) + " 次启动");
    }

    FMT_CHECK_EQ(read_lines(log_dir / "fmt.log").size(), std::size_t{2});
}

FMT_TEST(Logger, 目录自动创建) {
    fmt_test::TempDir temp("logger-mkdir");
    const auto nested = temp / "a" / "b";

    {
        auto opened = fmt::Logger::open(nested, quiet_options());
        FMT_CHECK(fmt::ok(opened));
    }

    FMT_CHECK(std::filesystem::is_directory(nested));
    FMT_CHECK(std::filesystem::exists(nested / "fmt.log"));
}

FMT_TEST(Logger, 仅控制台不写文件) {
    fmt_test::TempDir temp("logger-console");
    const auto log_dir = temp / "log";

    std::unique_ptr<fmt::Logger> logger = fmt::Logger::console_only(quiet_options());
    logger->info("Main", "只进控制台");
    logger->error("Main", "也不写文件");

    FMT_CHECK(!std::filesystem::exists(log_dir));
}

FMT_TEST(Logger, 静默日志器不做事) {
    std::unique_ptr<fmt::Logger> logger = fmt::Logger::silent();
    logger->info("Main", "没有输出");
    logger->warn("Main", "没有输出");
    logger->error("Main", "没有输出");
    FMT_CHECK(true);  // 只要不崩溃即可
}

FMT_TEST(Logger, 超过上限会轮转出一代) {
    fmt_test::TempDir temp("logger-rotate");
    fmt::Logger::Options options = quiet_options();
    options.max_log_bytes = 1024;  // 小到写几十行就会超

    auto logger = fmt::Logger::open(temp.path(), options);
    FMT_CHECK(fmt::ok(logger));
    if (!fmt::ok(logger)) {
        return;
    }

    // 每 kRotationCheckInterval 行检查一次大小，所以写够多行必然轮转过
    for (int i = 0; i < 400; ++i) {
        std::get<std::unique_ptr<fmt::Logger>>(logger)
            ->info("Test", "日志轮转测试行 " + std::to_string(i));
    }

    // 上一代在 fmt.log.1 里；当前文件是轮转之后重开的
    FMT_CHECK(fmt::file_exists(temp.path() / "fmt.log.1"));
    std::error_code code;
    const std::uintmax_t current = std::filesystem::file_size(temp.path() / "fmt.log", code);
    const std::uintmax_t previous = std::filesystem::file_size(temp.path() / "fmt.log.1", code);
    FMT_CHECK(!code);
    FMT_CHECK(previous > 0);
    FMT_CHECK(current > 0);
    // 只留一代，且两代加起来不超过「上限 + 一次检查间隔的量」太多
    FMT_CHECK(previous + current < 400 * 64);  // 远小于全部写入量，说明真的轮转过
    // 轮转发生时会在新文件里留一行说明，用户翻日志能看到断点
    FMT_CHECK(read_text(temp.path() / "fmt.log").find("轮转") != std::string::npos);
}

FMT_TEST(Logger, 上限为零时不轮转) {
    fmt_test::TempDir temp("logger-norotate");
    fmt::Logger::Options options = quiet_options();
    options.max_log_bytes = 0;  // 明确要求不轮转

    auto logger = fmt::Logger::open(temp.path(), options);
    FMT_CHECK(fmt::ok(logger));
    if (!fmt::ok(logger)) {
        return;
    }
    for (int i = 0; i < 200; ++i) {
        std::get<std::unique_ptr<fmt::Logger>>(logger)->info("Test", "不轮转测试行 " + std::to_string(i));
    }
    FMT_CHECK(!fmt::file_exists(temp.path() / "fmt.log.1"));
    // 全部行都在同一个文件里
    FMT_CHECK_EQ(read_lines(temp.path() / "fmt.log").size(), std::size_t{200});
}

// ---------------------------------------------------------------------------
// log/ 的清理（cleanup_logs）：轮转管「单个文件别太大」，它管「整个目录别太大」。
// ---------------------------------------------------------------------------

namespace {

// 造一份带行结构的日志内容：每行 64 字节左右。
std::string make_log_content(std::size_t lines, const std::string& tag) {
    std::string content;
    content.reserve(lines * 80);
    for (std::size_t i = 0; i < lines; ++i) {
        content += "2026-10-12 10:00:00 [INFO] [Test] " + tag + " 第 " +
                   std::to_string(i) + " 行" + std::string(40, 'x') + "\n";
    }
    return content;
}

std::uintmax_t size_of(const std::filesystem::path& path) {
    std::error_code code;
    return std::filesystem::file_size(path, code);
}

}  // namespace

FMT_TEST(Logger, 未超预算时一个字节都不动) {
    fmt_test::TempDir temp("logger-clean-under");
    const auto log_dir = temp / "log";
    FMT_CHECK(fmt::ok(fmt::ensure_directory(log_dir)));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(log_dir / "fmt.log", make_log_content(100, "a"))));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(log_dir / "fmt.log.1", make_log_content(50, "b"))));

    const std::uintmax_t before = size_of(log_dir / "fmt.log");
    const std::uintmax_t backup_before = size_of(log_dir / "fmt.log.1");

    const auto cleaned = fmt::cleanup_logs(log_dir, 1 * 1024 * 1024);  // 1 MB 预算，远大于占用
    FMT_CHECK(fmt::ok(cleaned));
    const fmt::LogCleanup& report = std::get<fmt::LogCleanup>(cleaned);
    FMT_CHECK(!report.over_budget);
    FMT_CHECK_EQ(report.removed_backups, std::size_t{0});
    FMT_CHECK_EQ(report.trimmed_files, std::size_t{0});
    FMT_CHECK_EQ(report.freed_bytes, std::uintmax_t{0});
    FMT_CHECK_EQ(report.message.find("未超过上限") != std::string::npos, true);

    // 备份也还在——「没超预算」就是不删不裁，连附加物都不动
    FMT_CHECK_EQ(size_of(log_dir / "fmt.log"), before);
    FMT_CHECK_EQ(size_of(log_dir / "fmt.log.1"), backup_before);
}

FMT_TEST(Logger, 超预算先删备份再裁活动文件) {
    fmt_test::TempDir temp("logger-clean-over");
    const auto log_dir = temp / "log";
    FMT_CHECK(fmt::ok(fmt::ensure_directory(log_dir)));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(log_dir / "fmt.log", make_log_content(4000, "a"))));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(log_dir / "fmt.log.1", make_log_content(2000, "b"))));
    // 用户手放进 log/ 的东西：清理一律不动
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(log_dir / "notes.txt", "用户自己的笔记")));

    const std::uintmax_t notes_before = size_of(log_dir / "notes.txt");
    const std::uintmax_t budget = 300 * 1024;

    const auto cleaned = fmt::cleanup_logs(log_dir, budget);
    FMT_CHECK(fmt::ok(cleaned));
    const fmt::LogCleanup& report = std::get<fmt::LogCleanup>(cleaned);
    FMT_CHECK(report.over_budget);
    FMT_CHECK_EQ(report.removed_backups, std::size_t{1});
    FMT_CHECK_EQ(report.trimmed_files, std::size_t{1});
    FMT_CHECK(report.freed_bytes > 0);
    // 报出来的占用必须与磁盘对得上（**别按字节精确断言**：安全软件/文件系统会在这
    // 两次 stat 之间碰一下文件，本机实测出现过 2 字节的抖动）。
    {
        const std::uintmax_t actual = size_of(log_dir / "fmt.log") + size_of(log_dir / "fmt.log.1") +
                                      size_of(log_dir / "error.log") + size_of(log_dir / "notes.txt");
        const std::uintmax_t diff =
            report.remaining_bytes > actual ? report.remaining_bytes - actual : actual - report.remaining_bytes;
        FMT_CHECK(diff <= 64);
    }

    // 备份没了；活动文件被裁到「预算的一半」，但不低于下限 kMinActiveLogBytes
    //（预算小到一半还不如下限时，保留下限——本用例的 300KB 预算正好落在这个区间）
    FMT_CHECK(!fmt::file_exists(log_dir / "fmt.log.1"));
    const std::uintmax_t keep = std::max(budget / 2, fmt::kMinActiveLogBytes);
    FMT_CHECK(size_of(log_dir / "fmt.log") <= keep + 512);  // +512：裁剪说明那一行
    FMT_CHECK(size_of(log_dir / "fmt.log") < 354890 - 512);  // 确实比裁剪前（约 346 KB）小
    const std::string trimmed = read_text(log_dir / "fmt.log");
    FMT_CHECK(trimmed.find("日志已裁剪") != std::string::npos);
    // 保留的是**最新**的一段：最后一行还在
    FMT_CHECK(trimmed.find("第 3999 行") != std::string::npos);
    FMT_CHECK(trimmed.find("第 0 行") == std::string::npos);

    // 别的文件一个字节都没动
    FMT_CHECK_EQ(size_of(log_dir / "notes.txt"), notes_before);
}

FMT_TEST(Logger, 预演只报告不动磁盘) {
    fmt_test::TempDir temp("logger-clean-dry");
    const auto log_dir = temp / "log";
    FMT_CHECK(fmt::ok(fmt::ensure_directory(log_dir)));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(log_dir / "fmt.log", make_log_content(4000, "a"))));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(log_dir / "fmt.log.1", make_log_content(2000, "b"))));

    const std::uintmax_t before = size_of(log_dir / "fmt.log");
    const std::uintmax_t backup_before = size_of(log_dir / "fmt.log.1");
    const std::uintmax_t budget = 300 * 1024;

    const auto cleaned = fmt::cleanup_logs(log_dir, budget, /*dry_run=*/true);
    FMT_CHECK(fmt::ok(cleaned));
    const fmt::LogCleanup& report = std::get<fmt::LogCleanup>(cleaned);
    FMT_CHECK(report.dry_run);
    FMT_CHECK(report.over_budget);
    FMT_CHECK_EQ(report.removed_backups, std::size_t{1});
    FMT_CHECK_EQ(report.trimmed_files, std::size_t{1});
    FMT_CHECK(report.freed_bytes > 0);

    // 磁盘一个字节都没动
    FMT_CHECK_EQ(size_of(log_dir / "fmt.log"), before);
    FMT_CHECK_EQ(size_of(log_dir / "fmt.log.1"), backup_before);
    FMT_CHECK(fmt::file_exists(log_dir / "fmt.log.1"));
}

FMT_TEST(Logger, 预算为零只删备份不裁活动文件) {
    fmt_test::TempDir temp("logger-clean-nobudget");
    const auto log_dir = temp / "log";
    FMT_CHECK(fmt::ok(fmt::ensure_directory(log_dir)));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(log_dir / "fmt.log", make_log_content(100, "a"))));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(log_dir / "error.log.1", make_log_content(50, "b"))));

    const std::uintmax_t active_before = size_of(log_dir / "fmt.log");

    const auto cleaned = fmt::cleanup_logs(log_dir, 0);  // 0 = 只删备份
    FMT_CHECK(fmt::ok(cleaned));
    const fmt::LogCleanup& report = std::get<fmt::LogCleanup>(cleaned);
    FMT_CHECK(report.over_budget);
    FMT_CHECK_EQ(report.removed_backups, std::size_t{1});
    FMT_CHECK_EQ(report.trimmed_files, std::size_t{0});
    FMT_CHECK(!fmt::file_exists(log_dir / "error.log.1"));
    FMT_CHECK_EQ(size_of(log_dir / "fmt.log"), active_before);
}

FMT_TEST(Logger, 日志目录不存在不是错误) {
    fmt_test::TempDir temp("logger-clean-missing");
    const auto cleaned = fmt::cleanup_logs(temp / "log", 1024 * 1024);
    FMT_CHECK(fmt::ok(cleaned));
    const fmt::LogCleanup& report = std::get<fmt::LogCleanup>(cleaned);
    FMT_CHECK(!report.over_budget);
    FMT_CHECK_EQ(report.removed_backups, std::size_t{0});
    FMT_CHECK_EQ(report.trimmed_files, std::size_t{0});
    // 不创建目录：清理不该在磁盘上留下东西
    FMT_CHECK(!std::filesystem::exists(temp / "log"));
}
