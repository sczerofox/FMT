// bucket 模块的单元测试
#include "fmt/bucket/bucket.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "fmt/core/app.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

namespace {

// 每个用例一个独立数据根：initialize_root 会把 current_user 置成占位名 "user"。
struct Fixture {
    fmt_test::TempDir temp{"bucket"};
    std::filesystem::path root = temp / "FMT";
    std::unique_ptr<fmt::PathManager> paths;
    fmt::Config config;

    Fixture() {
        auto initialized = fmt::initialize_root(root);
        paths = std::move(std::get<std::unique_ptr<fmt::PathManager>>(initialized));
        config = std::get<fmt::Config>(fmt::load_config(*paths));
    }

    std::filesystem::path repository(std::string_view bucket) const {
        return root / "repository" / "user" / fmt::path_from_utf8(std::string(bucket));
    }
};

// 往 file.json 追加一条记录（**追加**，不是重建：重建会把上一条冲掉，
// 「只标记本桶」的验证就失去意义了）。
void seed_file_record(const fmt::PathManager& paths, const std::string& bucket,
                      const std::string& file_name) {
    nlohmann::json document = fmt::make_collection(1, "files");
    if (fmt::file_exists(paths.file_data())) {
        const auto existing = fmt::read_json_file(paths.file_data());
        if (fmt::ok(existing)) {
            document = std::get<nlohmann::json>(existing);
        }
    }

    nlohmann::json record = nlohmann::json::object();
    record["file_id"] = "fmt-20261008-" + std::to_string(document["files"].size());
    record["user"] = "user";
    record["bucket"] = bucket;
    record["file_name"] = file_name;
    record["is_trash"] = false;
    document["files"].push_back(std::move(record));
    FMT_CHECK(fmt::ok(fmt::write_json_file(paths.file_data(), document)));
}

}  // namespace

FMT_TEST(Bucket, 首个Bucket自动成为当前) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    FMT_CHECK(fmt::ok(buckets.create("工作")));
    FMT_CHECK(fmt::directory_exists(f.repository("工作")));
    FMT_CHECK_EQ(f.config.current_bucket, std::string("工作"));

    // 第二个不会抢走「当前」
    FMT_CHECK(fmt::ok(buckets.create("生活")));
    FMT_CHECK_EQ(f.config.current_bucket, std::string("工作"));

    const auto list = buckets.list();
    FMT_CHECK(fmt::ok(list));
    const std::vector<fmt::BucketInfo>& items = std::get<std::vector<fmt::BucketInfo>>(list);
    FMT_CHECK_EQ(items.size(), std::size_t{2});
    FMT_CHECK_EQ(items[0].name, std::string("工作"));
    FMT_CHECK(items[0].is_current);
    FMT_CHECK(!items[1].is_current);
}

FMT_TEST(Bucket, 名称非法与重复创建被拒) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    const auto bad = buckets.create("a/b");
    FMT_CHECK(!fmt::ok(bad));
    FMT_CHECK(fmt::error_of(bad)->code == fmt::ErrorCode::BucketNameInvalid);
    FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(bad)->code), 2);

    FMT_CHECK(fmt::ok(buckets.create("工作")));
    const auto again = buckets.create("工作");
    FMT_CHECK(!fmt::ok(again));
    FMT_CHECK(fmt::error_of(again)->code == fmt::ErrorCode::BucketAlreadyExists);
    FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(again)->code), 4);
}

FMT_TEST(Bucket, use只改当前不动Bucket) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    FMT_CHECK(fmt::ok(buckets.create("生活")));

    FMT_CHECK(fmt::ok(buckets.use("生活")));
    FMT_CHECK_EQ(f.config.current_bucket, std::string("生活"));
    // 两个 Bucket 都还在
    FMT_CHECK(fmt::directory_exists(f.repository("工作")));
    FMT_CHECK(fmt::directory_exists(f.repository("生活")));

    // 配置真的落盘了
    FMT_CHECK_EQ(std::get<fmt::Config>(fmt::load_config(*f.paths)).current_bucket,
                 std::string("生活"));

    const auto missing = buckets.use("不存在");
    FMT_CHECK(!fmt::ok(missing));
    FMT_CHECK(fmt::error_of(missing)->code == fmt::ErrorCode::BucketNotFound);
    FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(missing)->code), 3);
}

FMT_TEST(Bucket, get返回名称与当前标记) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("工作")));

    const auto info = buckets.get("工作");
    FMT_CHECK(fmt::ok(info));
    FMT_CHECK_EQ(std::get<fmt::BucketInfo>(info).name, std::string("工作"));
    FMT_CHECK(std::get<fmt::BucketInfo>(info).is_current);

    const auto missing = buckets.get("不存在");
    FMT_CHECK(!fmt::ok(missing));
    FMT_CHECK(fmt::error_of(missing)->code == fmt::ErrorCode::BucketNotFound);
}

FMT_TEST(Bucket, 删除移入回收站并清空当前) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    seed_file_record(*f.paths, "工作", "a.txt");
    seed_file_record(*f.paths, "生活", "b.txt");  // 别的桶的记录不该被动

    const auto removal = buckets.remove("工作");
    FMT_CHECK(fmt::ok(removal));
    const fmt::BucketRemoval& result = std::get<fmt::BucketRemoval>(removal);
    FMT_CHECK(result.was_current);
    FMT_CHECK_EQ(result.files_affected, std::size_t{1});

    // 原目录没了，数据躺在回收站里（保持层级）
    FMT_CHECK(!fmt::directory_exists(f.repository("工作")));
    FMT_CHECK(fmt::directory_exists(result.moved_to));
    FMT_CHECK(fmt::directory_exists(f.root / "trash" / "user"));

    // 当前 Bucket 置空，且不自动切换到别的
    FMT_CHECK_EQ(f.config.current_bucket, std::string{});

    // file.json：本桶的记录 is_trash = true，别的桶不受影响
    const auto files = fmt::read_json_file(f.paths->file_data());
    FMT_CHECK(fmt::ok(files));
    const nlohmann::json& records = std::get<nlohmann::json>(files)["files"];
    for (const nlohmann::json& record : records) {
        if (record.value("bucket", std::string{}) == "工作") {
            FMT_CHECK(record.value("is_trash", false));
        } else {
            FMT_CHECK(!record.value("is_trash", true));
        }
    }

    // trash.json 有一条 Bucket 级记录（没有 file_id）
    const auto trash = fmt::read_json_file(f.paths->trash_data());
    FMT_CHECK(fmt::ok(trash));
    const nlohmann::json& entries = std::get<nlohmann::json>(trash)["trash"];
    FMT_CHECK_EQ(entries.size(), std::size_t{1});
    FMT_CHECK_EQ(entries[0].value("type", std::string{}), std::string("bucket"));
    FMT_CHECK_EQ(entries[0].value("bucket", std::string{}), std::string("工作"));
    FMT_CHECK(!entries[0].contains("file_id"));
    FMT_CHECK(entries[0].value("original_path", std::string{}).find("repository/user/工作") !=
              std::string::npos);
}

FMT_TEST(Bucket, 回收站同名不覆盖) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    FMT_CHECK(fmt::ok(buckets.create("工作")));
    const auto first = buckets.remove("工作");
    FMT_CHECK(fmt::ok(first));

    FMT_CHECK(fmt::ok(buckets.create("工作")));
    const auto second = buckets.remove("工作");
    FMT_CHECK(fmt::ok(second));

    // 两次删除的数据都要在，第二次换了名字
    FMT_CHECK(fmt::directory_exists(std::get<fmt::BucketRemoval>(first).moved_to));
    FMT_CHECK(fmt::directory_exists(std::get<fmt::BucketRemoval>(second).moved_to));
    FMT_CHECK(std::get<fmt::BucketRemoval>(first).moved_to !=
              std::get<fmt::BucketRemoval>(second).moved_to);

    const auto trash = fmt::read_json_file(f.paths->trash_data());
    FMT_CHECK(fmt::ok(trash));
    FMT_CHECK_EQ(std::get<nlohmann::json>(trash)["trash"].size(), std::size_t{2});
}

FMT_TEST(Bucket, 当前Bucket失效时置空) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    f.config.current_bucket = "早就没了";
    FMT_CHECK(fmt::ok(buckets.refresh_current_bucket()));
    FMT_CHECK_EQ(f.config.current_bucket, std::string{});

    // 只有一个 Bucket 时也绝不自动切换（第 61 节）
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    f.config.current_bucket = "早就没了";
    FMT_CHECK(fmt::ok(buckets.refresh_current_bucket()));
    FMT_CHECK_EQ(f.config.current_bucket, std::string{});
}

FMT_TEST(Bucket, 没有当前用户时拒绝) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    f.config.current_user.clear();
    const auto status = buckets.create("工作");
    FMT_CHECK(!fmt::ok(status));
    FMT_CHECK(fmt::error_of(status)->code == fmt::ErrorCode::NoCurrentUser);

    // 占位用户名由 initialize_root 自动补上，所以正常路径不会走到这里
    FMT_CHECK_EQ(std::get<fmt::Config>(fmt::load_config(*f.paths)).current_user,
                 std::string("user"));
}
