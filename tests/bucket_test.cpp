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

FMT_TEST(Bucket, 删除移入回收站名字带时间戳) {
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

    // 回收站里的名字 = 原桶名 + "_" + 14 位时间戳（YYYYMMDDHHMMSS）
    const std::string prefix = "工作_";
    FMT_CHECK_EQ(result.trashed_name.rfind(prefix, 0), std::size_t{0});
    FMT_CHECK_EQ(result.trashed_name.size(), prefix.size() + 14);
    for (std::size_t i = prefix.size(); i < result.trashed_name.size(); ++i) {
        FMT_CHECK(result.trashed_name[i] >= '0' && result.trashed_name[i] <= '9');
    }

    // 原目录没了，整棵树躺在回收站里（目录名就是回收站里的名字）
    FMT_CHECK(!fmt::directory_exists(f.repository("工作")));
    FMT_CHECK(fmt::directory_exists(result.moved_to));
    FMT_CHECK_EQ(fmt::path_to_utf8(result.moved_to.filename()), result.trashed_name);

    // 当前 Bucket 置空，且不自动切换到别的
    FMT_CHECK_EQ(f.config.current_bucket, std::string{});

    // file.json：本桶的记录 is_trash = true 且原因是 bucket，别的桶不受影响
    const auto files = fmt::read_json_file(f.paths->file_data());
    FMT_CHECK(fmt::ok(files));
    for (const nlohmann::json& record : std::get<nlohmann::json>(files)["files"]) {
        if (record.value("bucket", std::string{}) == "工作") {
            FMT_CHECK(record.value("is_trash", false));
            FMT_CHECK_EQ(record.value("trash_reason", std::string{}), std::string("bucket"));
        } else {
            FMT_CHECK(!record.value("is_trash", true));
        }
    }

    // .original 是桶级记录的权威：记了原名与回收站里的名字
    const auto index = fmt::read_json_file(f.root / "trash" / "user" / ".original");
    FMT_CHECK(fmt::ok(index));
    const nlohmann::json& entries = std::get<nlohmann::json>(index)["buckets"];
    FMT_CHECK_EQ(entries.size(), std::size_t{1});
    FMT_CHECK_EQ(entries[0].value("trashed", std::string{}), result.trashed_name);
    FMT_CHECK_EQ(entries[0].value("original", std::string{}), std::string("工作"));
    FMT_CHECK(!entries[0].value("deleted_at", std::string{}).empty());

    // 桶级记录只在 .original 里，不再重复写进 data/trash.json
    const auto trash = fmt::read_json_file(f.paths->trash_data());
    FMT_CHECK(fmt::ok(trash));
    FMT_CHECK_EQ(std::get<nlohmann::json>(trash)["trash"].size(), std::size_t{0});
}

FMT_TEST(Bucket, 同一秒删两次也不覆盖) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    FMT_CHECK(fmt::ok(buckets.create("工作")));
    const auto first = buckets.remove("工作");
    FMT_CHECK(fmt::ok(first));

    FMT_CHECK(fmt::ok(buckets.create("工作")));
    const auto second = buckets.remove("工作");
    FMT_CHECK(fmt::ok(second));

    const std::string first_name = std::get<fmt::BucketRemoval>(first).trashed_name;
    const std::string second_name = std::get<fmt::BucketRemoval>(second).trashed_name;
    FMT_CHECK(first_name != second_name);
    FMT_CHECK(fmt::directory_exists(std::get<fmt::BucketRemoval>(first).moved_to));
    FMT_CHECK(fmt::directory_exists(std::get<fmt::BucketRemoval>(second).moved_to));

    const auto index = fmt::read_json_file(f.root / "trash" / "user" / ".original");
    FMT_CHECK(fmt::ok(index));
    FMT_CHECK_EQ(std::get<nlohmann::json>(index)["buckets"].size(), std::size_t{2});
}

FMT_TEST(Bucket, 索引损坏时拒绝删除) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("工作")));

    // .original 坏了：原名记录不可信，宁可不删，也不能搬走一个「没有身份」的桶
    FMT_CHECK(fmt::ok(fmt::ensure_directory(f.root / "trash" / "user")));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(f.root / "trash" / "user" / ".original",
                                                  "{ 这不是 JSON")));

    const auto removal = buckets.remove("工作");
    FMT_CHECK(!fmt::ok(removal));
    FMT_CHECK(fmt::error_of(removal)->code == fmt::ErrorCode::JsonParseError);
    FMT_CHECK(fmt::directory_exists(f.repository("工作")));  // 一个字节都没动
}

FMT_TEST(Bucket, 回退成功与已存在拒绝) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    seed_file_record(*f.paths, "工作", "a.txt");

    const auto removal = buckets.remove("工作");
    FMT_CHECK(fmt::ok(removal));
    const std::string trashed = std::get<fmt::BucketRemoval>(removal).trashed_name;

    const auto listed = buckets.list_trashed();
    FMT_CHECK(fmt::ok(listed));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(listed).size(), std::size_t{1});
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(listed)[0].original_name,
                 std::string("工作"));

    // 原桶名唯一时也可以直接报原名
    const auto restored = buckets.restore("工作");
    FMT_CHECK(fmt::ok(restored));
    FMT_CHECK_EQ(std::get<fmt::TrashBucket>(restored).trashed_name, trashed);
    FMT_CHECK(fmt::directory_exists(f.repository("工作")));
    FMT_CHECK(!fmt::directory_exists(f.root / "trash" / "user" / fmt::path_from_utf8(trashed)));

    // 文件记录的 is_trash 翻回来，原因标记清掉
    const auto files = fmt::read_json_file(f.paths->file_data());
    FMT_CHECK(fmt::ok(files));
    for (const nlohmann::json& record : std::get<nlohmann::json>(files)["files"]) {
        FMT_CHECK(!record.value("is_trash", true));
        FMT_CHECK_EQ(record.value("trash_reason", std::string{}), std::string{});
    }

    const auto after = buckets.list_trashed();
    FMT_CHECK(fmt::ok(after));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(after).size(), std::size_t{0});

    // 目标已存在：restore 走的是**整目录搬回**那一条路，不是合并——
    // 目标在就拒绝（不改名、不覆盖），合并要显式调 `merge()`（第 56.3 节，
    // 命令层的 `trash restore` 由预检判定后转发到它）。
    FMT_CHECK(fmt::ok(buckets.remove("工作")));
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    const auto conflict = buckets.restore("工作");
    FMT_CHECK(!fmt::ok(conflict));
    FMT_CHECK(fmt::error_of(conflict)->code == fmt::ErrorCode::RestoreConflict);
    FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(conflict)->code), 4);

    // 被拒绝后：回收站那条还在，目标桶没被动
    const auto still = buckets.list_trashed();
    FMT_CHECK(fmt::ok(still));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(still).size(), std::size_t{1});
    FMT_CHECK(fmt::directory_exists(f.repository("工作")));

    // 这一条里已经没有文件（桶里的 a.txt 数据在 .files/ 下），所以合并不用搬东西，
    // 只需收尾：摘掉 .original、收掉空目录
    const auto merged = buckets.merge("工作");
    FMT_CHECK(fmt::ok(merged));
    FMT_CHECK_EQ(std::get<fmt::BucketMerge>(merged).moved_files, std::size_t{0});
    const auto done = buckets.list_trashed();
    FMT_CHECK(fmt::ok(done));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(done).size(), std::size_t{0});
    FMT_CHECK(fmt::directory_exists(f.repository("工作")));
}

FMT_TEST(Bucket, 同名多条回退要指定回收站名字) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    FMT_CHECK(fmt::ok(buckets.create("工作")));
    const auto first = buckets.remove("工作");
    FMT_CHECK(fmt::ok(first));
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    const auto second = buckets.remove("工作");
    FMT_CHECK(fmt::ok(second));

    // 按原名找：两条候选，必须让用户指定回收站名字
    const auto ambiguous = buckets.restore("工作");
    FMT_CHECK(!fmt::ok(ambiguous));
    FMT_CHECK(fmt::error_of(ambiguous)->code == fmt::ErrorCode::InvalidArgument);

    // 按回收站名字：精确命中，不多不少
    const auto restored = buckets.restore(std::get<fmt::BucketRemoval>(first).trashed_name);
    FMT_CHECK(fmt::ok(restored));
    const auto remaining = buckets.list_trashed();
    FMT_CHECK(fmt::ok(remaining));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(remaining).size(), std::size_t{1});
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(remaining)[0].trashed_name,
                 std::get<fmt::BucketRemoval>(second).trashed_name);
}

FMT_TEST(Bucket, 没有身份记录的目录只报告不回退) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    // 手工往回收站里放一个目录（索引里没有）
    FMT_CHECK(fmt::ok(fmt::ensure_directory(f.root / "trash" / "user" /
                                            fmt::path_from_utf8("孤儿_20260101000000"))));

    const auto listed = buckets.list_trashed();
    FMT_CHECK(fmt::ok(listed));
    const std::vector<fmt::TrashBucket>& entries = std::get<std::vector<fmt::TrashBucket>>(listed);
    FMT_CHECK_EQ(entries.size(), std::size_t{1});
    FMT_CHECK_EQ(entries[0].original_name, std::string{});  // 原名称未知
    FMT_CHECK(entries[0].directory_present);

    // 「剥掉时间戳猜原名」是不可靠的：拒绝回退，而不是猜一个位置
    const auto restored = buckets.restore("孤儿_20260101000000");
    FMT_CHECK(!fmt::ok(restored));
    FMT_CHECK(fmt::error_of(restored)->code == fmt::ErrorCode::InvalidArgument);
}

// 用户提的场景：删掉空的 lazy-fox -> 重新建 lazy-fox 放一个文件再删 ->
// 回退那个空的 -> 在回退回来的桶里放新文件 -> 再回退后面那个（带文件的）。
// 关键：两次删除的目录名不同（各带自己的时间戳），而回退是整单判定，
// 目标已存在就拒绝，两边的数据都不许被覆盖。
FMT_TEST(Bucket, 删空桶重建再删然后回退不会互相覆盖) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    // ① 删掉一个空的 lazy-fox
    FMT_CHECK(fmt::ok(buckets.create("lazy-fox")));
    const auto first = buckets.remove("lazy-fox");
    FMT_CHECK(fmt::ok(first));
    const std::string first_trashed = std::get<fmt::BucketRemoval>(first).trashed_name;

    // ② 重新建一个同名桶，放一个文件，再删
    FMT_CHECK(fmt::ok(buckets.create("lazy-fox")));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(f.repository("lazy-fox") / "xiaogg.txt", "old")));
    const auto second = buckets.remove("lazy-fox");
    FMT_CHECK(fmt::ok(second));
    const std::string second_trashed = std::get<fmt::BucketRemoval>(second).trashed_name;
    FMT_CHECK(first_trashed != second_trashed);

    // ③ 回退第一次那个（空的）
    FMT_CHECK(fmt::ok(buckets.restore(first_trashed)));
    FMT_CHECK(fmt::directory_exists(f.repository("lazy-fox")));

    // ④ 在回退回来的桶里放一个新文件
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(
        f.repository("lazy-fox") / fmt::path_from_utf8("新文件.txt"), "new")));

    // ⑤ 回退第二次那个（带 xiaogg.txt）：目标已存在 -> 整单拒绝
    const auto conflict = buckets.restore(second_trashed);
    FMT_CHECK(!fmt::ok(conflict));
    FMT_CHECK(fmt::error_of(conflict)->code == fmt::ErrorCode::RestoreConflict);

    // 两边的数据都完好：新桶里的新文件、回收站里的旧文件
    FMT_CHECK(fmt::file_exists(f.repository("lazy-fox") / fmt::path_from_utf8("新文件.txt")));
    FMT_CHECK(fmt::file_exists(f.root / "trash" / "user" / fmt::path_from_utf8(second_trashed) /
                               "xiaogg.txt"));
    // 被拒绝的那条仍然躺在回收站里等着处理
    const auto listed = buckets.list_trashed();
    FMT_CHECK(fmt::ok(listed));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(listed).size(), std::size_t{1});
}

FMT_TEST(Bucket, 目标桶已存在时回退是合并进去) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    // ① 桶里有东西（按日期树放）→ 删桶
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    seed_file_record(*f.paths, "工作", "a.txt");
    FMT_CHECK(fmt::ok(fmt::ensure_directory(f.repository("工作") / "2026" / "10" / "08")));
    FMT_CHECK(fmt::ok(
        fmt::write_text_file_atomic(f.repository("工作") / "2026" / "10" / "08" / "a.txt", "hello")));
    const auto removal = buckets.remove("工作");
    FMT_CHECK(fmt::ok(removal));
    const std::string trashed = std::get<fmt::BucketRemoval>(removal).trashed_name;

    // ② 原位置又有了同名桶（用户先单独回退过桶里某个文件时，file restore 会把空目录建出来）
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    FMT_CHECK(fmt::ok(fmt::ensure_directory(f.repository("工作") / "2026" / "10" / "08")));
    FMT_CHECK(fmt::ok(
        fmt::write_text_file_atomic(f.repository("工作") / "2026" / "10" / "08" / "b.txt", "keep")));

    // ③ 预检：不是「回退失败」，而是「合并进去」——说清搬几个、目标已有几个
    const auto checked = buckets.check_merge(trashed);
    FMT_CHECK(fmt::ok(checked));
    const fmt::BucketMergeCheck& check = std::get<fmt::BucketMergeCheck>(checked);
    FMT_CHECK(check.target_exists);
    FMT_CHECK_EQ(check.target_name, std::string("工作"));
    FMT_CHECK_EQ(check.move_files, std::size_t{1});
    FMT_CHECK_EQ(check.move_bytes, std::uintmax_t{5});
    FMT_CHECK_EQ(check.target_files, std::size_t{1});
    FMT_CHECK(check.conflicts.empty());

    // ④ 执行：a.txt 落到**它自己的日期目录**里，b.txt 一个字节都没动
    const auto merged = buckets.merge(trashed);
    FMT_CHECK(fmt::ok(merged));
    const fmt::BucketMerge& result = std::get<fmt::BucketMerge>(merged);
    FMT_CHECK_EQ(result.moved_files, std::size_t{1});
    FMT_CHECK_EQ(result.failed_files, std::size_t{0});
    FMT_CHECK_EQ(result.records_freed, std::size_t{1});
    FMT_CHECK(fmt::file_exists(f.repository("工作") / "2026" / "10" / "08" / "a.txt"));
    FMT_CHECK(fmt::file_exists(f.repository("工作") / "2026" / "10" / "08" / "b.txt"));
    FMT_CHECK(!fmt::file_exists(f.root / "trash" / "user" / fmt::path_from_utf8(trashed) /
                                "2026" / "10" / "08" / "a.txt"));

    // ⑤ 索引与记录都收尾了：条目没了、a.txt 不再是「已删除」
    const auto after = buckets.list_trashed();
    FMT_CHECK(fmt::ok(after));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(after).size(), std::size_t{0});
    const auto files = fmt::read_json_file(f.paths->file_data());
    FMT_CHECK(fmt::ok(files));
    for (const nlohmann::json& record : std::get<nlohmann::json>(files)["files"]) {
        FMT_CHECK(!record.value("is_trash", true));
        FMT_CHECK_EQ(record.value("trash_reason", std::string{}), std::string{});
    }
}

FMT_TEST(Bucket, 合并撞上同名文件就一个都不搬) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    FMT_CHECK(fmt::ok(buckets.create("工作")));
    seed_file_record(*f.paths, "工作", "a.txt");
    FMT_CHECK(fmt::ok(fmt::ensure_directory(f.repository("工作") / "2026" / "10" / "08")));
    FMT_CHECK(fmt::ok(
        fmt::write_text_file_atomic(f.repository("工作") / "2026" / "10" / "08" / "a.txt", "old")));
    const auto removal = buckets.remove("工作");
    FMT_CHECK(fmt::ok(removal));
    const std::string trashed = std::get<fmt::BucketRemoval>(removal).trashed_name;

    // 新桶里有一个同名文件（内容不同）——合并不覆盖，所以整单拒绝
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    FMT_CHECK(fmt::ok(fmt::ensure_directory(f.repository("工作") / "2026" / "10" / "08")));
    FMT_CHECK(fmt::ok(
        fmt::write_text_file_atomic(f.repository("工作") / "2026" / "10" / "08" / "a.txt", "new")));
    seed_file_record(*f.paths, "工作", "a.txt");

    const auto checked = buckets.check_merge(trashed);
    FMT_CHECK(fmt::ok(checked));
    const fmt::BucketMergeCheck& check = std::get<fmt::BucketMergeCheck>(checked);
    FMT_CHECK_EQ(check.conflicts.size(), std::size_t{1});
    FMT_CHECK_EQ(check.move_files, std::size_t{0});
    FMT_CHECK(check.message.find("不覆盖") != std::string::npos);

    // 确认也没用：这是冲突，不是「要不要做」的问题
    const auto merged = buckets.merge(trashed);
    FMT_CHECK(!fmt::ok(merged));
    FMT_CHECK(fmt::error_of(merged)->code == fmt::ErrorCode::RestoreMergeConflict);
    FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(merged)->code), 4);

    // 两边的数据都原样在：目标还是 "new"，回收站里还是 "old"
    const auto target_bytes =
        fmt::read_text_file(f.repository("工作") / "2026" / "10" / "08" / "a.txt");
    FMT_CHECK(fmt::ok(target_bytes));
    FMT_CHECK_EQ(std::get<std::string>(target_bytes), std::string("new"));
    const auto trash_bytes = fmt::read_text_file(f.root / "trash" / "user" /
                                                 fmt::path_from_utf8(trashed) / "2026" / "10" /
                                                 "08" / "a.txt");
    FMT_CHECK(fmt::ok(trash_bytes));
    FMT_CHECK_EQ(std::get<std::string>(trash_bytes), std::string("old"));

    // 条目还在回收站里等着处理
    const auto listed = buckets.list_trashed();
    FMT_CHECK(fmt::ok(listed));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(listed).size(), std::size_t{1});
}

FMT_TEST(Bucket, 合并要求目标桶的实际拼写与原桶名一致) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    FMT_CHECK(fmt::ok(buckets.create("work")));
    const auto removal = buckets.remove("work");
    FMT_CHECK(fmt::ok(removal));
    const std::string trashed = std::get<fmt::BucketRemoval>(removal).trashed_name;

    // 手工造一个拼写不同的同名桶：file.json 里记的是 "work"，路径按它推导，
    // 合并进 "WORK" 会让那些记录指向不存在的目录，所以直接拒绝。
    FMT_CHECK(fmt::ok(fmt::ensure_directory(f.root / "repository" / "user" / "WORK")));
    const auto checked = buckets.check_merge(trashed);
    FMT_CHECK(fmt::ok(checked));
    FMT_CHECK(std::get<fmt::BucketMergeCheck>(checked).message.find("拼写") != std::string::npos);

    const auto merged = buckets.merge(trashed);
    FMT_CHECK(!fmt::ok(merged));
    FMT_CHECK(fmt::error_of(merged)->code == fmt::ErrorCode::RestoreConflict);
}

FMT_TEST(Bucket, 回收站里没文件的条目合并只收尾) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    FMT_CHECK(fmt::ok(buckets.create("空桶")));
    const auto removal = buckets.remove("空桶");
    FMT_CHECK(fmt::ok(removal));
    const std::string trashed = std::get<fmt::BucketRemoval>(removal).trashed_name;

    // 目标桶已存在，而回收站条目是空的：没有文件要搬，摘掉索引即可
    FMT_CHECK(fmt::ok(buckets.create("空桶")));
    const auto checked = buckets.check_merge(trashed);
    FMT_CHECK(fmt::ok(checked));
    FMT_CHECK_EQ(std::get<fmt::BucketMergeCheck>(checked).move_files, std::size_t{0});

    const auto merged = buckets.merge(trashed);
    FMT_CHECK(fmt::ok(merged));
    FMT_CHECK_EQ(std::get<fmt::BucketMerge>(merged).moved_files, std::size_t{0});
    const auto after = buckets.list_trashed();
    FMT_CHECK(fmt::ok(after));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(after).size(), std::size_t{0});
    FMT_CHECK(fmt::directory_exists(f.repository("空桶")));
}

FMT_TEST(Bucket, 条目详情与永久删除) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    seed_file_record(*f.paths, "工作", "a.txt");
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(f.repository("工作") / "a.txt", "hello")));

    const auto removal = buckets.remove("工作");
    FMT_CHECK(fmt::ok(removal));
    const std::string trashed = std::get<fmt::BucketRemoval>(removal).trashed_name;

    // ---- trash get ----
    const auto detail = buckets.get_trashed(trashed);
    FMT_CHECK(fmt::ok(detail));
    const fmt::TrashBucketDetail& found = std::get<fmt::TrashBucketDetail>(detail);
    FMT_CHECK_EQ(found.bucket.trashed_name, trashed);
    FMT_CHECK_EQ(found.bucket.original_name, std::string("工作"));
    FMT_CHECK(found.bucket.directory_present);
    FMT_CHECK_EQ(found.file_count, std::size_t{1});
    FMT_CHECK_EQ(found.byte_count, std::uintmax_t{5});

    const auto missing = buckets.get_trashed("没有这个条目");
    FMT_CHECK(!fmt::ok(missing));
    FMT_CHECK(fmt::error_of(missing)->code == fmt::ErrorCode::TrashEntryNotFound);

    // ---- 永久删除 ----
    const auto purged = buckets.purge(trashed);
    FMT_CHECK(fmt::ok(purged));
    const fmt::TrashPurge& result = std::get<fmt::TrashPurge>(purged);
    FMT_CHECK_EQ(result.trashed_name, trashed);
    FMT_CHECK_EQ(result.original_name, std::string("工作"));
    FMT_CHECK_EQ(result.removed_files, std::size_t{1});
    FMT_CHECK_EQ(result.removed_records, std::size_t{1});

    // 磁盘、索引、file.json 都干净了
    FMT_CHECK(!fmt::directory_exists(f.root / "trash" / "user" / fmt::path_from_utf8(trashed)));
    const auto after = buckets.list_trashed();
    FMT_CHECK(fmt::ok(after));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(after).size(), std::size_t{0});
    const auto files = fmt::read_json_file(f.paths->file_data());
    FMT_CHECK(fmt::ok(files));
    FMT_CHECK_EQ(std::get<nlohmann::json>(files)["files"].size(), std::size_t{0});

    // 再删一次：条目没了
    const auto again = buckets.purge(trashed);
    FMT_CHECK(!fmt::ok(again));
    FMT_CHECK(fmt::error_of(again)->code == fmt::ErrorCode::TrashEntryNotFound);
}

FMT_TEST(Bucket, 永久删除只清桶级记录) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    // 一条「因桶被删」、一条「文件自己删的」
    seed_file_record(*f.paths, "工作", "a.txt");
    seed_file_record(*f.paths, "工作", "b.txt");

    const auto removal = buckets.remove("工作");
    FMT_CHECK(fmt::ok(removal));

    // 手工把 b.txt 改成文件级删除：永久删除桶的时候不能把它一起清掉
    {
        const auto parsed = fmt::read_json_file(f.paths->file_data());
        FMT_CHECK(fmt::ok(parsed));
        nlohmann::json document = std::get<nlohmann::json>(parsed);
        for (nlohmann::json& record : document["files"]) {
            if (record.value("file_name", std::string{}) == "b.txt") {
                record["trash_reason"] = "file";
            }
        }
        FMT_CHECK(fmt::ok(fmt::write_json_file(f.paths->file_data(), document)));
    }

    const auto purged = buckets.purge(std::get<fmt::BucketRemoval>(removal).trashed_name);
    FMT_CHECK(fmt::ok(purged));
    FMT_CHECK_EQ(std::get<fmt::TrashPurge>(purged).removed_records, std::size_t{1});

    const auto files = fmt::read_json_file(f.paths->file_data());
    FMT_CHECK(fmt::ok(files));
    const nlohmann::json& records = std::get<nlohmann::json>(files)["files"];
    FMT_CHECK_EQ(records.size(), std::size_t{1});
    FMT_CHECK_EQ(records[0].value("file_name", std::string{}), std::string("b.txt"));
}

FMT_TEST(Bucket, 回收站扫描只认桶级条目) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    // 文件级条目的落点（点开头，必须跳过）
    FMT_CHECK(fmt::ok(fmt::ensure_directory(
        f.root / "trash" / "user" / ".files" / fmt::path_from_utf8("工作") / "2026" / "10")));
    // 不像桶级条目的目录（名字里没有 14 位时间戳）也要跳过
    FMT_CHECK(fmt::ok(fmt::ensure_directory(f.root / "trash" / "user" / fmt::path_from_utf8("杂物"))));
    // 形状正确的手工目录：列出但原名称未知
    FMT_CHECK(fmt::ok(fmt::ensure_directory(
        f.root / "trash" / "user" / fmt::path_from_utf8("孤儿_20260101000000"))));

    const auto listed = buckets.list_trashed();
    FMT_CHECK(fmt::ok(listed));
    const std::vector<fmt::TrashBucket>& entries = std::get<std::vector<fmt::TrashBucket>>(listed);
    FMT_CHECK_EQ(entries.size(), std::size_t{1});
    FMT_CHECK_EQ(entries[0].trashed_name, std::string("孤儿_20260101000000"));
    FMT_CHECK_EQ(entries[0].original_name, std::string{});
}

FMT_TEST(Bucket, 有索引没目录的条目可以永久删掉) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    const auto removal = buckets.remove("工作");
    FMT_CHECK(fmt::ok(removal));
    const std::string trashed = std::get<fmt::BucketRemoval>(removal).trashed_name;

    // 模拟「目录被外部删了、索引还在」：list 如实报告，purge 仍然可用（否则这条永远清不掉）
    std::error_code code;
    std::filesystem::remove_all(f.root / "trash" / "user" / fmt::path_from_utf8(trashed), code);
    FMT_CHECK(!code);

    const auto listed = buckets.list_trashed();
    FMT_CHECK(fmt::ok(listed));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(listed).size(), std::size_t{1});
    FMT_CHECK(!std::get<std::vector<fmt::TrashBucket>>(listed)[0].directory_present);

    const auto purged = buckets.purge(trashed);
    FMT_CHECK(fmt::ok(purged));
    FMT_CHECK_EQ(std::get<fmt::TrashPurge>(purged).removed_files, std::size_t{0});
    const auto after = buckets.list_trashed();
    FMT_CHECK(fmt::ok(after));
    FMT_CHECK_EQ(std::get<std::vector<fmt::TrashBucket>>(after).size(), std::size_t{0});
}

FMT_TEST(Bucket, 大小写不同也认得同一个桶) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("work")));
    FMT_CHECK(fmt::ok(buckets.create("other")));  // 第二个不抢「当前」

    // Windows 的目录名不区分大小写：用大写切过去，指向的还是同一个桶
    FMT_CHECK(fmt::ok(buckets.use("WORK")));

    const auto list = buckets.list();
    FMT_CHECK(fmt::ok(list));
    for (const fmt::BucketInfo& info : std::get<std::vector<fmt::BucketInfo>>(list)) {
        // current_bucket 里存的是用户敲的拼写（WORK），但标记必须落在实际的目录上
        FMT_CHECK_EQ(info.is_current, info.name == std::string("work"));
    }
}

FMT_TEST(Bucket, 创建时大写会转成小写) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);

    const auto created = buckets.create("WORK");
    FMT_CHECK(fmt::ok(created));
    if (!fmt::ok(created)) {
        return;
    }
    const fmt::BucketCreation& result = std::get<fmt::BucketCreation>(created);
    FMT_CHECK_EQ(result.requested, std::string("WORK"));
    FMT_CHECK_EQ(result.name, std::string("work"));
    FMT_CHECK(result.renamed);
    FMT_CHECK(result.became_current);
    FMT_CHECK_EQ(f.config.current_bucket, std::string("work"));

    // 磁盘上的**实际条目名**是小写（目录名本身不区分大小写，不能靠 directory_exists 判断）
    bool found_lower = false;
    bool found_upper = false;
    std::error_code code;
    for (const auto& entry :
         std::filesystem::directory_iterator(f.root / "repository" / "user", code)) {
        const std::string name = fmt::path_to_utf8(entry.path().filename());
        found_lower = found_lower || name == "work";
        found_upper = found_upper || name == "WORK";
    }
    FMT_CHECK(found_lower);
    FMT_CHECK(!found_upper);

    // 再敲大写会被认成同一个桶
    const auto again = buckets.create("WORK");
    FMT_CHECK(!fmt::ok(again));
    FMT_CHECK(fmt::error_of(again)->code == fmt::ErrorCode::BucketAlreadyExists);
}

FMT_TEST(Bucket, use大写规范化到磁盘上的名字) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("work")));

    FMT_CHECK(fmt::ok(buckets.use("WORK")));
    // 存进 current_bucket 的是磁盘上的实际拼写，不是用户敲的
    FMT_CHECK_EQ(f.config.current_bucket, std::string("work"));

    const auto info = buckets.get("WORK");
    FMT_CHECK(fmt::ok(info));
    FMT_CHECK_EQ(std::get<fmt::BucketInfo>(info).name, std::string("work"));
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
