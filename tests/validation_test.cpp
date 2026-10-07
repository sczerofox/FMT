// common/validation 的单元测试
#include "fmt/common/validation.hpp"

#include <string>

#include "fmt_test.hpp"

namespace {

void expect_bucket_name_rejected(const std::string& name) {
    const fmt::Status status = fmt::validate_bucket_name(name);
    FMT_CHECK(!fmt::ok(status));
    if (!fmt::ok(status)) {
        FMT_CHECK(fmt::error_of(status)->code == fmt::ErrorCode::BucketNameInvalid);
    }
}

void expect_file_name_rejected(const std::string& name, fmt::ErrorCode expected) {
    const fmt::Status status = fmt::validate_file_name(name);
    FMT_CHECK(!fmt::ok(status));
    if (!fmt::ok(status)) {
        FMT_CHECK(fmt::error_of(status)->code == expected);
    }
}

}  // namespace

FMT_TEST(Validation, Bucket名称可用) {
    for (const char* name : {"工作", "life", "a-b_c", "2026", "bucket.with.dot",
                             "小谷姐姐麻辣烫"}) {
        FMT_CHECK(fmt::ok(fmt::validate_bucket_name(name)));
    }
    // 边界：255 字节正好可用，256 太长
    FMT_CHECK(fmt::ok(fmt::validate_bucket_name(std::string(255, 'x'))));
}

FMT_TEST(Validation, Bucket名称被拒) {
    expect_bucket_name_rejected("");
    expect_bucket_name_rejected(std::string(256, 'x'));
    expect_bucket_name_rejected("a/b");
    expect_bucket_name_rejected("a\\b");
    expect_bucket_name_rejected(".");
    expect_bucket_name_rejected("..");
    expect_bucket_name_rejected("bad:name");
    expect_bucket_name_rejected("bad*name");
    expect_bucket_name_rejected("结尾空格 ");
    expect_bucket_name_rejected("结尾点.");
    expect_bucket_name_rejected(std::string("含\n换行"));
    expect_bucket_name_rejected("CON");
    expect_bucket_name_rejected("con");
    expect_bucket_name_rejected("Com1");
    expect_bucket_name_rejected("LPT9");
}

FMT_TEST(Validation, 保留设备名识别) {
    FMT_CHECK(fmt::is_windows_reserved_name("CON"));
    FMT_CHECK(fmt::is_windows_reserved_name("con"));
    FMT_CHECK(fmt::is_windows_reserved_name("Com1"));
    FMT_CHECK(fmt::is_windows_reserved_name("LPT9.txt"));
    FMT_CHECK(!fmt::is_windows_reserved_name("CONSOLE"));
    FMT_CHECK(!fmt::is_windows_reserved_name("COM0"));
    FMT_CHECK(!fmt::is_windows_reserved_name("工作"));
}

FMT_TEST(Validation, 文件名可用) {
    for (const char* name : {"a.txt", "小谷姐姐麻辣烫.jpg", "test.tar.gz", "a b.txt",
                             "2026-10-08.log"}) {
        FMT_CHECK(fmt::ok(fmt::validate_file_name(name)));
    }
}

FMT_TEST(Validation, 文件名错误码分工) {
    expect_file_name_rejected("", fmt::ErrorCode::FileNameEmpty);
    expect_file_name_rejected("a/b.txt", fmt::ErrorCode::FileNameSeparator);
    expect_file_name_rejected("a\\b.txt", fmt::ErrorCode::FileNameSeparator);
    expect_file_name_rejected("..", fmt::ErrorCode::FileNameSeparator);
    expect_file_name_rejected("a:b.txt", fmt::ErrorCode::FileNameInvalidChar);
    expect_file_name_rejected("a?b.txt", fmt::ErrorCode::FileNameInvalidChar);
    expect_file_name_rejected("NUL", fmt::ErrorCode::FileNameReserved);
    expect_file_name_rejected("con.txt", fmt::ErrorCode::FileNameReserved);
    expect_file_name_rejected(std::string(256, 'a'), fmt::ErrorCode::FileNameTooLong);
}

FMT_TEST(Validation, 路径穿越形状被拒) {
    // 用户输入 "../test.txt" 这种必须在上传前就被挡住（开发文档第 19、25 节）
    expect_file_name_rejected("../test.txt", fmt::ErrorCode::FileNameSeparator);
    expect_file_name_rejected("test/name.txt", fmt::ErrorCode::FileNameSeparator);
    expect_bucket_name_rejected("../../etc");
}
