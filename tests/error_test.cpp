// common/error 的单元测试
#include "fmt/common/error.hpp"

#include <string>
#include <variant>

#include "fmt_test.hpp"

FMT_TEST(ErrorCode, 编号转字符串) {
    FMT_CHECK_EQ(fmt::code_string(fmt::ErrorCode::InvalidArgument), std::string("FMT-001"));
    FMT_CHECK_EQ(fmt::code_string(fmt::ErrorCode::PermissionDenied), std::string("FMT-004"));
    FMT_CHECK_EQ(fmt::code_string(fmt::ErrorCode::JsonUnsupportedVersion), std::string("FMT-011"));
    FMT_CHECK_EQ(fmt::code_string(fmt::ErrorCode::FileNameConflict), std::string("FMT-105"));
    FMT_CHECK_EQ(fmt::code_string(fmt::ErrorCode::ServiceNotInstalled), std::string("FMT-601"));
    FMT_CHECK_EQ(fmt::code_string(fmt::ErrorCode::AdminRequired), std::string("FMT-603"));
}

FMT_TEST(ErrorCode, 字符串还原编号) {
    bool ok = false;
    FMT_CHECK(fmt::code_from_string("FMT-004", &ok) == fmt::ErrorCode::PermissionDenied);
    FMT_CHECK(ok);

    FMT_CHECK(fmt::code_from_string("FMT-601", &ok) == fmt::ErrorCode::ServiceNotInstalled);
    FMT_CHECK(ok);

    // 只给数字也认
    FMT_CHECK(fmt::code_from_string("603", &ok) == fmt::ErrorCode::AdminRequired);
    FMT_CHECK(ok);

    // 枚举名也认
    FMT_CHECK(fmt::code_from_string("Md5Duplicate", &ok) == fmt::ErrorCode::Md5Duplicate);
    FMT_CHECK(ok);

    // 未发布的编号必须失败，不能被当成合法错误
    FMT_CHECK(fmt::code_from_string("FMT-999", &ok) == fmt::ErrorCode::InvalidArgument);
    FMT_CHECK(!ok);

    FMT_CHECK(fmt::code_from_string("", &ok) == fmt::ErrorCode::InvalidArgument);
    FMT_CHECK(!ok);
}

FMT_TEST(ErrorCode, 字符串往返一致) {
    for (int number : {1, 4, 15, 105, 203, 305, 402, 503, 603, 701}) {
        const auto code = static_cast<fmt::ErrorCode>(number);
        bool ok = false;
        const fmt::ErrorCode restored = fmt::code_from_string(fmt::code_string(code), &ok);
        FMT_CHECK(ok);
        FMT_CHECK(restored == code);
    }
}

FMT_TEST(ErrorCode, 退出码映射) {
    FMT_CHECK_EQ(fmt::exit_code(fmt::ErrorCode::Ok), 0);
    FMT_CHECK_EQ(fmt::exit_code(fmt::ErrorCode::InvalidArgument), 2);
    FMT_CHECK_EQ(fmt::exit_code(fmt::ErrorCode::FileNotFound), 3);
    FMT_CHECK_EQ(fmt::exit_code(fmt::ErrorCode::FileAlreadyExists), 4);
    FMT_CHECK_EQ(fmt::exit_code(fmt::ErrorCode::PermissionDenied), 5);
    FMT_CHECK_EQ(fmt::exit_code(fmt::ErrorCode::JsonParseError), 6);
    FMT_CHECK_EQ(fmt::exit_code(fmt::ErrorCode::ConfigError), 7);
    FMT_CHECK_EQ(fmt::exit_code(fmt::ErrorCode::ServiceNotInstalled), 8);

    // 未映射的编号默认 1（通用错误）
    FMT_CHECK_EQ(fmt::exit_code(static_cast<fmt::ErrorCode>(999)), 1);
}

FMT_TEST(ErrorCode, 默认消息非空) {
    FMT_CHECK(!fmt::default_message(fmt::ErrorCode::PermissionDenied).empty());
    FMT_CHECK(!fmt::default_message(static_cast<fmt::ErrorCode>(999)).empty());
    FMT_CHECK_EQ(std::string(fmt::default_message(fmt::ErrorCode::PermissionDenied)),
                 std::string("权限不足"));
}

FMT_TEST(ErrorValue, 成功与错误互斥) {
    const fmt::Status good = std::monostate{};
    FMT_CHECK(fmt::ok(good));
    FMT_CHECK(fmt::error_of(good) == nullptr);

    const fmt::Status bad = fmt::make_error(fmt::ErrorCode::ServiceNotInstalled);
    FMT_CHECK(!fmt::ok(bad));
    FMT_CHECK(fmt::error_of(bad) != nullptr);
    FMT_CHECK(fmt::error_of(bad)->code == fmt::ErrorCode::ServiceNotInstalled);
    FMT_CHECK_EQ(fmt::error_of(bad)->message, std::string("服务未安装"));

    const fmt::Result<int> value = 42;
    FMT_CHECK(fmt::ok(value));
    FMT_CHECK(std::get<int>(value) == 42);

    const fmt::Result<int> failure = fmt::make_error(fmt::ErrorCode::BucketNotFound, "没有这个桶");
    FMT_CHECK(!fmt::ok(failure));
    FMT_CHECK_EQ(fmt::error_of(failure)->message, std::string("没有这个桶"));
}

FMT_TEST(ErrorValue, 自定义消息覆盖默认消息) {
    const fmt::Error error = fmt::make_error(fmt::ErrorCode::IoError, "写 config.json 失败");
    FMT_CHECK_EQ(error.message, std::string("写 config.json 失败"));
    FMT_CHECK_EQ(fmt::code_string(error.code), std::string("FMT-005"));
    FMT_CHECK_EQ(fmt::exit_code(error.code), 1);
}
