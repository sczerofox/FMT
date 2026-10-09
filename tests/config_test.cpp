// config 的单元测试
#include "fmt/config/config.hpp"

#include <filesystem>
#include <string>

#include "fmt/common/string.hpp"
#include "fmt/core/path_manager.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

FMT_TEST(Config, 首次运行写出默认配置) {
    fmt_test::TempDir temp("config-default");
    const fmt::PathManager paths{temp.path()};

    const auto config = fmt::load_config(paths);
    FMT_CHECK(fmt::ok(config));

    const fmt::Config& value = std::get<fmt::Config>(config);
    FMT_CHECK_EQ(value.version, 1);
    FMT_CHECK_EQ(value.current_user, std::string{});
    FMT_CHECK_EQ(value.current_bucket, std::string{});
    FMT_CHECK_EQ(value.max_upload_size, std::uint64_t{52428800});
    FMT_CHECK_EQ(value.size_unit, std::string("MB"));
    FMT_CHECK_EQ(value.language, std::string("zh-CN"));

    // 文件必须真的被写出来
    FMT_CHECK(fmt::file_exists(paths.config_file()));
}

FMT_TEST(Config, 保存后读回一致) {
    fmt_test::TempDir temp("config-roundtrip");
    const fmt::PathManager paths{temp.path()};

    fmt::Config saved;
    saved.current_user = "小谷";
    saved.current_bucket = "工作";
    saved.max_upload_size = 10485760;
    FMT_CHECK(fmt::ok(fmt::save_config(paths, saved)));

    const auto loaded = fmt::load_config(paths);
    FMT_CHECK(fmt::ok(loaded));
    FMT_CHECK_EQ(std::get<fmt::Config>(loaded).current_user, std::string("小谷"));
    FMT_CHECK_EQ(std::get<fmt::Config>(loaded).current_bucket, std::string("工作"));
    FMT_CHECK_EQ(std::get<fmt::Config>(loaded).max_upload_size, std::uint64_t{10485760});
}

FMT_TEST(Config, 损坏时不重置原文件) {
    fmt_test::TempDir temp("config-broken");
    const fmt::PathManager paths{temp.path()};
    FMT_CHECK(fmt::ok(fmt::ensure_directory(paths.config())));

    const std::string broken = "{\"version\":1,\"current_user\":}";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(paths.config_file(), broken)));

    const auto config = fmt::load_config(paths);
    FMT_CHECK(!fmt::ok(config));
    FMT_CHECK(fmt::error_of(config)->code == fmt::ErrorCode::JsonParseError);
    FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(config)->code), 6);

    // 冻结规则：JSON 损坏不能静默重置
    FMT_CHECK_EQ(std::get<std::string>(fmt::read_text_file(paths.config_file())), broken);
}

FMT_TEST(Config, 未知版本被拒绝) {
    fmt_test::TempDir temp("config-version");
    const fmt::PathManager paths{temp.path()};
    FMT_CHECK(fmt::ok(fmt::ensure_directory(paths.config())));
    FMT_CHECK(fmt::ok(
        fmt::write_text_file_atomic(paths.config_file(), "{\"version\":2,\"language\":\"zh-CN\"}")));

    const auto config = fmt::load_config(paths);
    FMT_CHECK(!fmt::ok(config));
    FMT_CHECK(fmt::error_of(config)->code == fmt::ErrorCode::JsonUnsupportedVersion);
    FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(config)->code), 6);
}

FMT_TEST(Config, 字段缺失补齐并回写) {
    fmt_test::TempDir temp("config-patch");
    const fmt::PathManager paths{temp.path()};
    FMT_CHECK(fmt::ok(fmt::ensure_directory(paths.config())));
    // 只有 version 和 language，其余字段缺失；类型也错一个
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(
        paths.config_file(), "{\"version\":1,\"language\":\"en-US\",\"max_upload_size\":\"big\"}")));

    const auto config = fmt::load_config(paths);
    FMT_CHECK(fmt::ok(config));
    FMT_CHECK_EQ(std::get<fmt::Config>(config).language, std::string("en-US"));   // 有效值保留
    FMT_CHECK_EQ(std::get<fmt::Config>(config).max_upload_size, std::uint64_t{52428800});  // 类型错，回落

    // 补齐结果必须写回文件，第二次读不能再触发补齐
    const auto text = fmt::read_text_file(paths.config_file());
    FMT_CHECK(fmt::ok(text));
    FMT_CHECK(std::get<std::string>(text).find("size_unit") != std::string::npos);
}

FMT_TEST(Config, 服务配置默认值) {
    fmt_test::TempDir temp("config-server");
    const fmt::PathManager paths{temp.path()};

    const auto server = fmt::load_server_config(paths);
    FMT_CHECK(fmt::ok(server));
    FMT_CHECK_EQ(std::get<fmt::ServerConfig>(server).enabled, false);
    FMT_CHECK_EQ(std::get<fmt::ServerConfig>(server).host, std::string("localhost"));
    FMT_CHECK_EQ(std::get<fmt::ServerConfig>(server).port, 4122);

    fmt::ServerConfig enabled;
    enabled.enabled = true;
    enabled.port = 4123;
    FMT_CHECK(fmt::ok(fmt::save_server_config(paths, enabled)));

    const auto reloaded = fmt::load_server_config(paths);
    FMT_CHECK(fmt::ok(reloaded));
    FMT_CHECK_EQ(std::get<fmt::ServerConfig>(reloaded).enabled, true);
    FMT_CHECK_EQ(std::get<fmt::ServerConfig>(reloaded).port, 4123);
    FMT_CHECK_EQ(std::get<fmt::ServerConfig>(reloaded).host, std::string("localhost"));
}
