// storage 的单元测试
#include "fmt/storage/storage.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "fmt/common/string.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

FMT_TEST(Storage, 两个写者同时写同一个文件不会互相踩) {
    fmt_test::TempDir temp("storage-concurrent");
    const auto target = temp / "state.json";

    // 同一时刻有多个写者写同一个目标，这是**真实发生**的：
    // `service install` 会在启动服务之后写 service.json，而服务启动时也写它。
    // 旧的实现固定用 `<目标>.tmp`，先完成的一方把它 rename 走，另一方做读回校验时
    // 文件已经不在 → 报「无法打开文件 …tmp」，两边都可能失败（安装器那处还忽略了
    // 返回值），结果就是 service.json 静默地一直不更新。
    std::atomic<int> failures{0};
    std::vector<std::thread> writers;
    for (int writer = 0; writer < 8; ++writer) {
        writers.emplace_back([&target, &failures, writer] {
            for (int round = 0; round < 40; ++round) {
                const nlohmann::json value{{"writer", writer}, {"round", round}};
                if (!ok(fmt::write_json_file(target, value))) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }

    FMT_CHECK_EQ(failures.load(), 0);

    // 内容必须是**某一次完整写入**的结果（不能是两份内容交错的产物）
    const auto parsed = fmt::read_json_file(target);
    FMT_CHECK(ok(parsed));
    if (ok(parsed)) {
        const nlohmann::json& value = std::get<nlohmann::json>(parsed);
        FMT_CHECK(value.contains("writer"));
        FMT_CHECK(value.contains("round"));
        FMT_CHECK(value["writer"].get<int>() >= 0 && value["writer"].get<int>() < 8);
        FMT_CHECK(value["round"].get<int>() >= 0 && value["round"].get<int>() < 40);
    }

    // 临时文件不能留下来
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(temp.path(), code)) {
        FMT_CHECK(entry.path().extension() != ".tmp");
    }
}

FMT_TEST(Storage, 文本原子写与读回) {
    fmt_test::TempDir temp("storage-text");
    const auto path = temp / "config.json";

    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(path, "{\"version\":1}\n")));

    const auto text = fmt::read_text_file(path);
    FMT_CHECK(fmt::ok(text));
    FMT_CHECK_EQ(std::get<std::string>(text), std::string("{\"version\":1}\n"));

    // 临时文件必须被替换掉，不能留在磁盘上
    FMT_CHECK(!std::filesystem::exists(temp / "config.json.tmp"));

    // 覆盖写也要成功
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(path, "second\n")));
    FMT_CHECK_EQ(std::get<std::string>(fmt::read_text_file(path)), std::string("second\n"));
}

FMT_TEST(Storage, 读不存在的文件) {
    fmt_test::TempDir temp("storage-missing");
    const auto result = fmt::read_text_file(temp / "nope.json");
    FMT_CHECK(!fmt::ok(result));
    FMT_CHECK(fmt::error_of(result)->code == fmt::ErrorCode::FileNotFound);
}

FMT_TEST(Storage, JSON读写) {
    fmt_test::TempDir temp("storage-json");
    const auto path = temp / "server.json";

    nlohmann::json value = nlohmann::json::object();
    value["version"] = 1;
    value["enabled"] = true;
    value["host"] = "127.0.0.1";
    value["port"] = 4122;
    FMT_CHECK(fmt::ok(fmt::write_json_file(path, value)));

    const auto loaded = fmt::read_json_file(path);
    FMT_CHECK(fmt::ok(loaded));
    FMT_CHECK_EQ(std::get<nlohmann::json>(loaded)["port"].get<int>(), 4122);
    FMT_CHECK_EQ(std::get<nlohmann::json>(loaded)["enabled"].get<bool>(), true);
}

FMT_TEST(Storage, 中文原样写入不转义) {
    fmt_test::TempDir temp("storage-utf8");
    const auto path = temp / "user.json";

    nlohmann::json value = nlohmann::json::object();
    value["version"] = 1;
    value["current_user"] = "小谷";
    FMT_CHECK(fmt::ok(fmt::write_json_file(path, value)));

    const std::string text = std::get<std::string>(fmt::read_text_file(path));
    FMT_CHECK(text.find("小谷") != std::string::npos);
    FMT_CHECK(text.find("\\u") == std::string::npos);
}

FMT_TEST(Storage, 损坏的JSON报格式错误且不改动原文件) {
    fmt_test::TempDir temp("storage-broken");
    const auto path = temp / "file.json";
    const std::string broken = "{\"version\":1,\"files\":[}";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(path, broken)));

    const auto result = fmt::read_json_file(path);
    FMT_CHECK(!fmt::ok(result));
    FMT_CHECK(fmt::error_of(result)->code == fmt::ErrorCode::JsonParseError);

    // 文档冻结项：JSON 损坏不能静默重置，原文件必须一字不动
    FMT_CHECK_EQ(std::get<std::string>(fmt::read_text_file(path)), broken);
}

FMT_TEST(Storage, 版本策略) {
    nlohmann::json good = nlohmann::json::object();
    good["version"] = 1;
    FMT_CHECK(fmt::ok(fmt::check_version(good, 1)));

    nlohmann::json future = nlohmann::json::object();
    future["version"] = 2;
    const fmt::Status unsupported = fmt::check_version(future, 1);
    FMT_CHECK(!fmt::ok(unsupported));
    FMT_CHECK(fmt::error_of(unsupported)->code == fmt::ErrorCode::JsonUnsupportedVersion);

    nlohmann::json missing = nlohmann::json::object();
    const fmt::Status no_version = fmt::check_version(missing, 1);
    FMT_CHECK(!fmt::ok(no_version));
    FMT_CHECK(fmt::error_of(no_version)->code == fmt::ErrorCode::JsonParseError);

    const fmt::Status not_object = fmt::check_version(nlohmann::json::array(), 1);
    FMT_CHECK(!fmt::ok(not_object));
    FMT_CHECK(fmt::error_of(not_object)->code == fmt::ErrorCode::JsonParseError);
}

FMT_TEST(Storage, 集合默认结构) {
    const nlohmann::json files = fmt::make_collection(1, "files");
    FMT_CHECK(files["version"].get<int>() == 1);
    FMT_CHECK(files["files"].is_array());
    FMT_CHECK(files["files"].empty());

    const nlohmann::json users = fmt::make_collection(1, "users");
    FMT_CHECK(users.contains("users"));
}

FMT_TEST(Storage, 目录创建幂等) {
    fmt_test::TempDir temp("storage-dir");
    const auto nested = temp / "a" / "b" / "c";

    FMT_CHECK(fmt::ok(fmt::ensure_directory(nested)));
    FMT_CHECK(fmt::directory_exists(nested));

    // 再来一次也必须成功
    FMT_CHECK(fmt::ok(fmt::ensure_directory(nested)));

    FMT_CHECK(fmt::path_exists(nested));
    FMT_CHECK(!fmt::file_exists(nested));
}
