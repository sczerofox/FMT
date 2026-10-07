// cli 的单元测试：命令行切分、提权辅助函数
#include "fmt/cli/cli.hpp"

#include <string>
#include <vector>

#include "fmt_test.hpp"

FMT_TEST(Cli, 命令切分) {
    const std::vector<std::string> simple = fmt::cli::split_command("service stop");
    FMT_CHECK_EQ(simple.size(), std::size_t{2});
    FMT_CHECK_EQ(simple[0], std::string("service"));
    FMT_CHECK_EQ(simple[1], std::string("stop"));

    const std::vector<std::string> spaced = fmt::cli::split_command("   file    list   ");
    FMT_CHECK_EQ(spaced.size(), std::size_t{2});
    FMT_CHECK_EQ(spaced[0], std::string("file"));
    FMT_CHECK_EQ(spaced[1], std::string("list"));

    const std::vector<std::string> empty = fmt::cli::split_command("    ");
    FMT_CHECK(empty.empty());

    // 带空格的参数用引号包起来
    const std::vector<std::string> quoted =
        fmt::cli::split_command("file upload \"D:\\my files\\a b.txt\"");
    FMT_CHECK_EQ(quoted.size(), std::size_t{3});
    FMT_CHECK_EQ(quoted[2], std::string("D:\\my files\\a b.txt"));

    // 中文参数
    const std::vector<std::string> chinese = fmt::cli::split_command("bucket create 工作");
    FMT_CHECK_EQ(chinese.size(), std::size_t{3});
    FMT_CHECK_EQ(chinese[2], std::string("工作"));
}

FMT_TEST(Cli, 管道输入带BOM也能识别) {
    // 重定向/管道进来的第一行常见带上 UTF-8 BOM
    const std::vector<std::string> with_bom = fmt::cli::split_command("\xEF\xBB\xBF" "exit");
    FMT_CHECK_EQ(with_bom.size(), std::size_t{1});
    FMT_CHECK_EQ(with_bom[0], std::string("exit"));
}

FMT_TEST(Cli, 提权结果文件路径) {
    const std::string path = fmt::cli::result_file_for(4242);
    FMT_CHECK(path.find("fmt-elev-4242.json") != std::string::npos);
}

FMT_TEST(Cli, 提权判断不崩溃) {
    // 测试进程通常未提权；即使提权了，也只要求它能正常回答。
    const bool elevated = fmt::cli::is_elevated();
    FMT_CHECK(elevated == true || elevated == false);
}
