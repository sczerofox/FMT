// core/app 初始化的单元测试
#include "fmt/core/app.hpp"

#include <filesystem>
#include <string>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"
FMT_TEST(App, 初始化数据根会建默认账号与token) {
    fmt_test::TempDir temp("app-user");
    const auto root = temp / "root";

    const auto paths = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(paths));
    if (!fmt::ok(paths)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(paths);

    // 直接看文件：初始化之后 data/user.json 里就该有一个默认用户
    const auto document = fmt::read_json_file(manager.user_data());
    FMT_CHECK(fmt::ok(document));
    if (!fmt::ok(document)) {
        return;
    }
    const nlohmann::json& users = std::get<nlohmann::json>(document)["users"];
    FMT_CHECK_EQ(users.size(), std::size_t{1});
    if (users.empty()) {
        return;
    }

    FMT_CHECK_EQ(users[0].value("username", std::string{}), std::string("user"));
    FMT_CHECK(!users[0].value("user_id", std::string{}).empty());
    // token：32 位十六进制（16 字节 CSPRNG），永久有效
    const std::string token = users[0].value("token", std::string{});
    FMT_CHECK_EQ(token.size(), std::size_t{32});
    FMT_CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
    // 密码只存哈希与盐（PBKDF2-SHA256 -> 64 位十六进制）
    FMT_CHECK_EQ(users[0].value("password_hash", std::string{}).size(), std::size_t{64});
    FMT_CHECK(!users[0].value("password_salt", std::string{}).empty());
    FMT_CHECK(!users[0].value("created_at", std::string{}).empty());

    // 再初始化一次：不覆盖已有账号（token 与创建时间都不变）
    const auto again = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(again));
    const auto after = fmt::read_json_file(manager.user_data());
    FMT_CHECK(fmt::ok(after));
    if (fmt::ok(after)) {
        const nlohmann::json& kept = std::get<nlohmann::json>(after)["users"];
        FMT_CHECK_EQ(kept.size(), std::size_t{1});
        if (!kept.empty()) {
            FMT_CHECK_EQ(kept[0].value("token", std::string{}), token);
            FMT_CHECK_EQ(kept[0].value("created_at", std::string{}),
                         users[0].value("created_at", std::string{}));
        }
    }
}

FMT_TEST(App, 已有空users文件时也要补建默认账号) {
    // 复刻线上状态：数据根早就初始化过，data/user.json 是 `{"users":[],"version":1}`
    //（那会儿还没有账号功能）。这时候再初始化必须把默认账号补上。
    fmt_test::TempDir temp("app-user-existing");
    const auto root = temp / "root";

    const auto first = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(first));
    if (!fmt::ok(first)) {
        return;
    }
    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(first);

    // 手工把它清回「老数据根」的样子
    FMT_CHECK(fmt::ok(fmt::write_json_file(manager.user_data(), fmt::make_collection(1, "users"))));

    const auto second = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(second));
    if (!fmt::ok(second)) {
        return;
    }

    const auto document = fmt::read_json_file(manager.user_data());
    FMT_CHECK(fmt::ok(document));
    if (fmt::ok(document)) {
        const nlohmann::json& users = std::get<nlohmann::json>(document)["users"];
        FMT_CHECK_EQ(users.size(), std::size_t{1});
        if (!users.empty()) {
            FMT_CHECK_EQ(users[0].value("username", std::string{}), std::string("user"));
            FMT_CHECK_EQ(users[0].value("token", std::string{}).size(), std::size_t{32});
        }
    }
}

FMT_TEST(App, 初始化建出六个目录与默认JSON) {
    fmt_test::TempDir temp("app-init");
    const auto root = temp / "FMT";

    const auto paths = fmt::initialize_root(root);
    FMT_CHECK(fmt::ok(paths));

    for (const std::string& name : fmt::PathManager::required_directories()) {
        FMT_CHECK(fmt::directory_exists(root / fmt::path_from_utf8(name)));
    }

    const fmt::PathManager& manager = *std::get<std::unique_ptr<fmt::PathManager>>(paths);
    FMT_CHECK(fmt::file_exists(manager.config_file()));
    FMT_CHECK(fmt::file_exists(manager.server_file()));
    FMT_CHECK(fmt::file_exists(manager.file_data()));
    FMT_CHECK(fmt::file_exists(manager.share_data()));
    FMT_CHECK(fmt::file_exists(manager.trash_data()));
    FMT_CHECK(fmt::file_exists(manager.user_data()));

    // 集合文件的默认结构
    const auto files = fmt::read_json_file(manager.file_data());
    FMT_CHECK(fmt::ok(files));
    FMT_CHECK_EQ(std::get<nlohmann::json>(files)["version"].get<int>(), 1);
    FMT_CHECK(std::get<nlohmann::json>(files)["files"].is_array());
}

FMT_TEST(App, 初始化幂等且不碰已有数据) {
    fmt_test::TempDir temp("app-idempotent");
    const auto root = temp / "FMT";
    const fmt::PathManager paths{root};

    {
        const auto first = fmt::initialize_root(root);
        FMT_CHECK(fmt::ok(first));
    }

    // 人为放进「用户数据」与一份已经有内容的配置
    const auto marker = root / "repository" / "小谷.txt";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(marker, "用户数据，不许动")));

    fmt::Config existing;
    existing.current_user = "小谷";
    existing.current_bucket = "工作";
    existing.max_upload_size = 10485760;
    FMT_CHECK(fmt::ok(fmt::save_config(paths, existing)));

    {
        const auto second = fmt::initialize_root(root);
        FMT_CHECK(fmt::ok(second));
    }

    // 用户数据一字不动
    FMT_CHECK(std::filesystem::exists(marker));
    FMT_CHECK_EQ(std::get<std::string>(fmt::read_text_file(marker)), std::string("用户数据，不许动"));

    // 已有配置的字段被保留（不是被默认值覆盖回去）
    const auto reloaded = fmt::load_config(paths);
    FMT_CHECK(fmt::ok(reloaded));
    FMT_CHECK_EQ(std::get<fmt::Config>(reloaded).current_user, std::string("小谷"));
    FMT_CHECK_EQ(std::get<fmt::Config>(reloaded).current_bucket, std::string("工作"));
    FMT_CHECK_EQ(std::get<fmt::Config>(reloaded).max_upload_size, std::uint64_t{10485760});
}

FMT_TEST(App, 检查能报出缺失的目录与文件) {
    fmt_test::TempDir temp("app-check");
    const fmt::PathManager paths{temp / "FMT"};

    const fmt::RootReport report = fmt::check_root(paths);
    FMT_CHECK(!report.complete());
    FMT_CHECK_EQ(report.missing_directories.size(), std::size_t{6});
    // 四个集合文件 + config.json + server.json
    FMT_CHECK_EQ(report.missing_files.size(), std::size_t{6});
    FMT_CHECK(report.broken_files.empty());
}

FMT_TEST(App, 补齐只补缺失且幂等) {
    fmt_test::TempDir temp("app-ensure");
    const auto root = temp / "FMT";
    const fmt::PathManager paths{root};

    const auto first = fmt::ensure_root(paths);
    FMT_CHECK(fmt::ok(first));
    FMT_CHECK_EQ(std::get<fmt::RootRepair>(first).created_directories.size(), std::size_t{6});
    FMT_CHECK_EQ(std::get<fmt::RootRepair>(first).created_files.size(), std::size_t{6});
    FMT_CHECK(fmt::check_root(paths).complete());

    // 再补一次：什么都不该再建
    const auto second = fmt::ensure_root(paths);
    FMT_CHECK(fmt::ok(second));
    FMT_CHECK(std::get<fmt::RootRepair>(second).created_directories.empty());
    FMT_CHECK(std::get<fmt::RootRepair>(second).created_files.empty());

    // 用户自己放的文件不受影响
    const auto mine = root / fmt::path_from_utf8("说明.txt");
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(mine, "别删我")));
    FMT_CHECK(fmt::ok(fmt::ensure_root(paths)));
    FMT_CHECK(fmt::file_exists(mine));
}

FMT_TEST(App, 损坏的JSON只报告不修复) {
    fmt_test::TempDir temp("app-broken-check");
    const auto root = temp / "FMT";
    const fmt::PathManager paths{root};
    FMT_CHECK(fmt::ok(fmt::ensure_root(paths)));

    const std::string broken = "{\"version\":1,\"files\":[}";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(paths.file_data(), broken)));

    const fmt::RootReport report = fmt::check_root(paths);
    FMT_CHECK(!report.complete());
    FMT_CHECK(report.missing_files.empty());
    FMT_CHECK_EQ(report.broken_files.size(), std::size_t{1});
    FMT_CHECK(report.broken_files[0].find("FMT-006") != std::string::npos);

    // 补齐不会碰它：冻结规则是「损坏的 JSON 不静默重置」
    FMT_CHECK(fmt::ok(fmt::ensure_root(paths)));
    FMT_CHECK_EQ(std::get<std::string>(fmt::read_text_file(paths.file_data())), broken);
    FMT_CHECK_EQ(fmt::check_root(paths).broken_files.size(), std::size_t{1});
}

FMT_TEST(App, 服务上下文打开日志并加载配置) {
    fmt_test::TempDir temp("app-service");
    const auto root = temp / "FMT";

    const auto context = fmt::initialize_service_context(root);
    FMT_CHECK(fmt::ok(context));

    fmt::AppContext& app = *std::get<std::unique_ptr<fmt::AppContext>>(context);
    FMT_CHECK(app.paths != nullptr);
    FMT_CHECK(app.logger != nullptr);
    FMT_CHECK_EQ(app.config.language, std::string("zh-CN"));
    FMT_CHECK_EQ(app.server_config.port, 4122);

    // 日志目录与文件都已建立，并且写入了数据根信息
    FMT_CHECK(fmt::file_exists(app.paths->log() / "fmt.log"));
    const auto log = fmt::read_text_file(app.paths->log() / "fmt.log");
    FMT_CHECK(fmt::ok(log));
    FMT_CHECK(std::get<std::string>(log).find("数据根") != std::string::npos);
}

FMT_TEST(App, 损坏的配置让初始化失败) {
    fmt_test::TempDir temp("app-bad-config");
    const auto root = temp / "FMT";
    FMT_CHECK(fmt::ok(fmt::ensure_directory(root / "config")));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(root / "config" / "config.json", "{ broken")));

    const auto context = fmt::initialize_service_context(root);
    FMT_CHECK(!fmt::ok(context));
    FMT_CHECK(fmt::error_of(context)->code == fmt::ErrorCode::JsonParseError);
}
