// common/logger 的单元测试
#include "fmt/common/logger.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

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
