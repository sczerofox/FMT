// common/string 的单元测试
#include "fmt/common/string.hpp"

#include <string>
#include <vector>

#include "fmt_test.hpp"

FMT_TEST(String, 清理粘贴带进来的路径污染) {
    // U+202A ... U+202C：从聊天窗口、网页、终端复制路径时夹进来的方向格式字符。
    // 屏幕上完全看不出来，拼进路径就让 exists() 说「文件不存在」。
    const std::string wrapped = std::string("\xE2\x80\xAA") + "C:\\a\\b.jpg" + "\xE2\x80\xAC";
    FMT_CHECK_EQ(fmt::clean_user_path(wrapped), std::string("C:\\a\\b.jpg"));

    // Explorer 的「复制路径」套的引号：**成对**才去掉
    FMT_CHECK_EQ(fmt::clean_user_path("\"C:\\a\\b.jpg\""), std::string("C:\\a\\b.jpg"));
    FMT_CHECK_EQ(fmt::clean_user_path("\"C:\\a\\b.jpg"), std::string("\"C:\\a\\b.jpg"));
    FMT_CHECK_EQ(fmt::clean_user_path("  C:\\a\\b.jpg\r\n"), std::string("C:\\a\\b.jpg"));

    // 不换行空格 U+00A0（网页复制常见）
    FMT_CHECK_EQ(fmt::clean_user_path(std::string("C:\\a\xC2\xA0\\b.jpg")),
                 std::string("C:\\a\\b.jpg"));

    // 中文路径不受影响（合法多字节序列原样保留）
    const std::string chinese = "C:\\图片\\头像\\asdva.jpg";
    FMT_CHECK_EQ(fmt::clean_user_path(chinese), chinese);

    // 诊断：点出码位并去重
    const std::vector<std::string> hidden = fmt::invisible_characters(wrapped);
    FMT_CHECK_EQ(hidden.size(), std::size_t{2});
    FMT_CHECK_EQ(hidden[0], std::string("U+202A"));
    FMT_CHECK_EQ(hidden[1], std::string("U+202C"));
    FMT_CHECK_EQ(fmt::invisible_characters("C:\\a\\b.jpg").size(), std::size_t{0});
}

FMT_TEST(String, UTF8与UTF16互转) {
    const std::string chinese = "小谷姐姐麻辣烫.jpg";
    const std::string restored = fmt::to_utf8(fmt::to_wide(chinese));
    FMT_CHECK_EQ(restored, chinese);

    FMT_CHECK_EQ(fmt::to_utf8(std::wstring_view{}), std::string{});
    FMT_CHECK_EQ(fmt::to_wide(std::string_view{}), std::wstring{});

    // 每个汉字 3 字节
    FMT_CHECK_EQ(fmt::to_wide("小谷").size(), std::size_t{2});
    FMT_CHECK_EQ(fmt::to_utf8(L"小谷").size(), std::size_t{6});
}

FMT_TEST(String, 前后缀与大小写) {
    FMT_CHECK(fmt::starts_with("FMT-004", "FMT-"));
    FMT_CHECK(!fmt::starts_with("FMT-", "FMT-004"));
    FMT_CHECK(fmt::ends_with("test.txt", ".txt"));
    FMT_CHECK(fmt::starts_with("anything", ""));

    FMT_CHECK(fmt::iequals("Service", "service"));
    FMT_CHECK(fmt::iequals("INSTALL", "install"));
    FMT_CHECK(!fmt::iequals("install", "uninstall"));

    FMT_CHECK_EQ(fmt::to_lower("FMT-ABC"), std::string("fmt-abc"));
    // 中文不能被改写
    FMT_CHECK_EQ(fmt::to_lower("中文ABC"), std::string("中文abc"));
}

FMT_TEST(String, 去空白) {
    FMT_CHECK_EQ(fmt::trim("  fmt >  "), std::string("fmt >"));
    FMT_CHECK_EQ(fmt::trim("\t\r\n"), std::string{});
    FMT_CHECK_EQ(fmt::trim("abc"), std::string("abc"));
}

FMT_TEST(String, 切分) {
    const std::vector<std::string> parts = fmt::split("file,list,now", ',');
    FMT_CHECK_EQ(parts.size(), std::size_t{3});
    FMT_CHECK_EQ(parts[0], std::string("file"));
    FMT_CHECK_EQ(parts[2], std::string("now"));

    // 保留空字段，调用方自己决定丢弃
    const std::vector<std::string> sparse = fmt::split("a,,b", ',');
    FMT_CHECK_EQ(sparse.size(), std::size_t{3});
    FMT_CHECK_EQ(sparse[1], std::string{});

    const std::vector<std::string> single = fmt::split("only", ',');
    FMT_CHECK_EQ(single.size(), std::size_t{1});
}

FMT_TEST(String, 文件大小显示) {
    FMT_CHECK_EQ(fmt::format_size(0), std::string("0B"));
    FMT_CHECK_EQ(fmt::format_size(512), std::string("512B"));
    FMT_CHECK_EQ(fmt::format_size(1024), std::string("1KB"));
    FMT_CHECK_EQ(fmt::format_size(1536), std::string("1.5KB"));
    FMT_CHECK_EQ(fmt::format_size(6ULL * 1024 * 1024), std::string("6MB"));
    FMT_CHECK_EQ(fmt::format_size(67ULL * 1024 * 1024), std::string("67MB"));
    FMT_CHECK_EQ(fmt::format_size(1024ULL * 1024 * 1024), std::string("1GB"));
    // 需求里的例子：文件大小 6MB
    FMT_CHECK_EQ(fmt::format_size(6291456), std::string("6MB"));
}

FMT_TEST(String, 路径分隔符归一) {
    FMT_CHECK_EQ(fmt::to_forward_slashes("repository\\小谷\\工作\\test.txt"),
                 std::string("repository/小谷/工作/test.txt"));
    FMT_CHECK_EQ(fmt::to_forward_slashes("already/fine"), std::string("already/fine"));
}

FMT_TEST(String, URL百分号编解码) {
    // 中文名走 HTTP 路径时必须能被还原
    FMT_CHECK_EQ(fmt::url_encode("工作"), std::string("%E5%B7%A5%E4%BD%9C"));
    FMT_CHECK_EQ(fmt::url_decode("%E5%B7%A5%E4%BD%9C"), std::string("工作"));
    FMT_CHECK_EQ(fmt::url_decode(fmt::url_encode("小谷姐姐麻辣烫")), std::string("小谷姐姐麻辣烫"));

    // unreserved 字符保持原样
    FMT_CHECK_EQ(fmt::url_encode("a-b_c.d~e"), std::string("a-b_c.d~e"));
    FMT_CHECK_EQ(fmt::url_encode("a b"), std::string("a%20b"));
    FMT_CHECK_EQ(fmt::url_encode("/"), std::string("%2F"));

    // 非法转义原样保留，不猜
    FMT_CHECK_EQ(fmt::url_decode("100%"), std::string("100%"));
    FMT_CHECK_EQ(fmt::url_decode("%ZZ"), std::string("%ZZ"));
    FMT_CHECK_EQ(fmt::url_decode("%2"), std::string("%2"));
    // 路径里的 '+' 不是空格
    FMT_CHECK_EQ(fmt::url_decode("a+b"), std::string("a+b"));
}
