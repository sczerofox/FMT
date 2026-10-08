// cli 的单元测试：命令行切分、提权辅助函数
#include "fmt/cli/cli.hpp"

#include <string>
#include <vector>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt_test.hpp"

FMT_TEST(Cli, 版本文本只有一个来源) {
    // 横幅、`--version`、`version` 命令都调 version_text()：分开写就会漂移
    //（以前窗口里敲 version 是「未知命令」，而 --version 能用）。
    const std::string text = fmt::cli::version_text();
    FMT_CHECK(text.find("File Manager Tool") != std::string::npos);
    FMT_CHECK(text.find("v1.0") != std::string::npos);
    FMT_CHECK(text.find("build") != std::string::npos);
    // 构建日期由 CMake 在配置时生成，所以只断言长度，不钉具体日期
    FMT_CHECK(text.size() > 30);
}

FMT_TEST(Cli, 位置参数的信封形状) {
    const nlohmann::json positional = nlohmann::json::array({"a7.jpg"});

    // 旧写法（把开关直接挂到位置参数数组上）会抛 type_error.305；
    // 未捕获就是用户看到的「Debug Error! abort() has been called」弹窗。
    nlohmann::json broken = positional;
    bool threw = false;
    try {
        broken["dry_run"] = true;
    } catch (const nlohmann::json::exception&) {
        threw = true;
    }
    FMT_CHECK(threw);

    // 新写法：位置参数在 argv，开关与它**同级**
    const nlohmann::json check = fmt::cli::argument_envelope(positional, /*dry_run=*/true);
    FMT_CHECK(check.is_object());
    FMT_CHECK(check.contains("argv"));
    FMT_CHECK_EQ(check["argv"][0].get<std::string>(), std::string("a7.jpg"));
    FMT_CHECK(check.value("dry_run", false));
    FMT_CHECK(!check.contains("force"));

    const nlohmann::json real =
        fmt::cli::argument_envelope(positional, /*dry_run=*/false, /*force=*/true);
    FMT_CHECK(real.contains("argv"));
    FMT_CHECK(real.value("force", false));
    FMT_CHECK(!real.contains("dry_run"));

    // 没有位置参数的命令（file list）也要拿到合法对象
    const nlohmann::json empty = fmt::cli::argument_envelope(nlohmann::json::array(), false, true);
    FMT_CHECK(empty.is_object());
    FMT_CHECK(!empty.contains("argv"));
    FMT_CHECK(empty.value("force", false));
}

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

FMT_TEST(Cli, 提权结果文件放在数据根的temp下) {
    const std::string path = fmt::cli::result_file_for(4242);
    FMT_CHECK(path.find("fmt-elev-4242.json") != std::string::npos);
    FMT_CHECK(path.find("temp") != std::string::npos);

    // 就在本进程 exe 所在目录的 temp/ 里：临时文件跟着 exe 走
    const std::string root = fmt::to_forward_slashes(fmt::path_to_utf8(fmt::executable_directory()));
    const std::string normalized = fmt::to_forward_slashes(path);
    FMT_CHECK(fmt::starts_with(normalized, root));
    FMT_CHECK(normalized.find("/temp/fmt-elev-") != std::string::npos);
}

FMT_TEST(Cli, 提权判断不崩溃) {
    // 测试进程通常未提权；即使提权了，也只要求它能正常回答。
    const bool elevated = fmt::cli::is_elevated();
    FMT_CHECK(elevated == true || elevated == false);
}
