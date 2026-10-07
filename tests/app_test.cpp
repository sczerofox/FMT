// core/app 初始化的单元测试
#include "fmt/core/app.hpp"

#include <filesystem>
#include <string>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

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
