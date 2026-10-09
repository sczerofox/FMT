// core/path_manager 的单元测试
#include "fmt/core/path_manager.hpp"

#include <filesystem>
#include <string>

#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"
#include "fmt/core/path.hpp"
#include "fmt_test.hpp"

namespace {

fmt::DateParts fixed_date() { return fmt::DateParts{"2026", "10", "05"}; }

std::string as_utf8(const fmt::Result<std::filesystem::path>& result) {
    return fmt::to_forward_slashes(fmt::path_to_utf8(std::get<std::filesystem::path>(result)));
}

}  // namespace

FMT_TEST(PathManager, 目录清单固定) {
    const std::vector<std::string>& directories = fmt::PathManager::required_directories();
    FMT_CHECK_EQ(directories.size(), std::size_t{6});
    FMT_CHECK_EQ(directories[0], std::string("repository"));
    FMT_CHECK_EQ(directories[1], std::string("trash"));
    FMT_CHECK_EQ(directories[2], std::string("config"));
    FMT_CHECK_EQ(directories[3], std::string("data"));
    FMT_CHECK_EQ(directories[4], std::string("log"));
    FMT_CHECK_EQ(directories[5], std::string("temp"));
}

FMT_TEST(PathManager, 根与子路径) {
    const fmt::PathManager paths{R"(D:\FMT2)"};
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.root()), std::string(R"(D:\FMT2)"));
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.repository()), std::string(R"(D:\FMT2\repository)"));
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.trash()), std::string(R"(D:\FMT2\trash)"));
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.log()), std::string(R"(D:\FMT2\log)"));
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.temp()), std::string(R"(D:\FMT2\temp)"));
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.config_file()),
                 std::string(R"(D:\FMT2\config\config.json)"));
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.server_file()),
                 std::string(R"(D:\FMT2\config\server.json)"));
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.file_data()), std::string(R"(D:\FMT2\data\file.json)"));
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.share_data()), std::string(R"(D:\FMT2\data\share.json)"));
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.trash_data()), std::string(R"(D:\FMT2\data\trash.json)"));
    FMT_CHECK_EQ(fmt::path_to_utf8(paths.user_data()), std::string(R"(D:\FMT2\data\user.json)"));
}

FMT_TEST(PathManager, 仓库路径按日期分层) {
    const fmt::PathManager paths{R"(D:\FMT)"};
    const auto result = paths.repository_file("小谷", "工作", fixed_date(), "test.txt");
    FMT_CHECK(fmt::ok(result));
    FMT_CHECK_EQ(as_utf8(result),
                 std::string("D:/FMT/repository/小谷/工作/2026/10/05/test.txt"));
}

FMT_TEST(PathManager, 回收站保持原层级) {
    const fmt::PathManager paths{R"(D:\FMT)"};
    const auto result = paths.trash_file("小谷", "工作", fixed_date(), "小谷姐姐麻辣烫.jpg");
    FMT_CHECK(fmt::ok(result));
    // 文件级条目收在保留的 .files/ 下：trash/<用户>/ 的顶层留给桶级条目
    // （trash/<用户>/<桶名>_<时间戳>/），两者不同层，桶级扫描不会误认。
    FMT_CHECK_EQ(as_utf8(result),
                 std::string("D:/FMT/trash/小谷/.files/工作/2026/10/05/小谷姐姐麻辣烫.jpg"));
}

FMT_TEST(PathManager, 拒绝路径穿越与分隔符) {
    const fmt::PathManager paths{R"(D:\FMT)"};

    // 文件名不能带路径分隔符
    const auto separated = paths.repository_file("小谷", "工作", fixed_date(), "a/b.txt");
    FMT_CHECK(!fmt::ok(separated));
    FMT_CHECK(fmt::error_of(separated)->code == fmt::ErrorCode::FileNameSeparator);

    // .. 是路径穿越
    const auto escaped = paths.repository_file("小谷", "工作", fixed_date(), "..");
    FMT_CHECK(!fmt::ok(escaped));
    FMT_CHECK(fmt::error_of(escaped)->code == fmt::ErrorCode::PathEscape);

    // 空片段
    const auto empty_user = paths.repository_file("", "工作", fixed_date(), "a.txt");
    FMT_CHECK(!fmt::ok(empty_user));
    FMT_CHECK(fmt::error_of(empty_user)->code == fmt::ErrorCode::InvalidArgument);

    const auto empty_bucket = paths.repository_file("小谷", "", fixed_date(), "a.txt");
    FMT_CHECK(!fmt::ok(empty_bucket));
    FMT_CHECK(fmt::error_of(empty_bucket)->code == fmt::ErrorCode::InvalidArgument);
}

FMT_TEST(PathManager, 拒绝非法日期) {
    const fmt::PathManager paths{R"(D:\FMT)"};
    const auto bad = paths.repository_file("小谷", "工作", fmt::DateParts{"26", "10", "05"}, "a.txt");
    FMT_CHECK(!fmt::ok(bad));
    FMT_CHECK(fmt::error_of(bad)->code == fmt::ErrorCode::InvalidArgument);
}
