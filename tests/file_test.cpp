// file 模块的单元测试
#include "fmt/file/file.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <cpp-httplib/httplib.h>

#include "fmt/bucket/bucket.hpp"
#include "fmt/common/hash.hpp"
#include "fmt/common/string.hpp"
#include "fmt/core/app.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

namespace {

constexpr std::uintmax_t kSizeLimit = 1024 * 1024;

struct Fixture {
    fmt_test::TempDir temp{"file"};
    std::filesystem::path root = temp / "FMT";
    std::unique_ptr<fmt::PathManager> paths;
    fmt::Config config;
    std::filesystem::path source = temp / "source.txt";

    Fixture() {
        paths = std::move(std::get<std::unique_ptr<fmt::PathManager>>(fmt::initialize_root(root)));
        config = std::get<fmt::Config>(fmt::load_config(*paths));

        FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(source, "hello world")));
        fmt::BucketService buckets(*paths, config, nullptr);
        FMT_CHECK(fmt::ok(buckets.create("工作")));  // 第一个桶自动成为当前
    }

    // 今天入库会落到的目录：repository/user/工作/YYYY/MM/DD
    std::filesystem::path today_directory() const {
        const std::filesystem::path base = root / "repository" / "user" / fmt::path_from_utf8("工作");
        std::error_code code;
        for (const auto& year : std::filesystem::directory_iterator(base, code)) {
            for (const auto& month : std::filesystem::directory_iterator(year.path(), code)) {
                for (const auto& day : std::filesystem::directory_iterator(month.path(), code)) {
                    return day.path();
                }
            }
        }
        return {};
    }
};

// 上传一个本地文件（两段式走完），返回记录。
fmt::Result<fmt::FileRecord> upload_local(Fixture& f, const std::string& name = {}) {
    fmt::FileService files(*f.paths, f.config, nullptr);
    auto prepared = fmt::prepare_upload(*f.paths, fmt::path_to_utf8(f.source), name, kSizeLimit, nullptr);
    if (!fmt::ok(prepared)) {
        return *fmt::error_of(prepared);
    }
    fmt::PreparedUpload upload = std::get<fmt::PreparedUpload>(prepared);
    return files.commit_upload(upload);
}

}  // namespace

FMT_TEST(File, 从来源推断文件名) {
    FMT_CHECK_EQ(fmt::file_name_from_source("https://example.com/a/b/test.zip"),
                 std::string("test.zip"));
    FMT_CHECK_EQ(fmt::file_name_from_source("http://example.com/a.bin?token=1#x"),
                 std::string("a.bin"));
    FMT_CHECK_EQ(fmt::file_name_from_source("http://example.com/dir/"), std::string("dir"));
    // URL 里的中文是百分号编码的，要还原
    FMT_CHECK_EQ(fmt::file_name_from_source("http://example.com/%E5%B7%A5%E4%BD%9C.txt"),
                 std::string("工作.txt"));
    // 本地路径
    FMT_CHECK_EQ(fmt::file_name_from_source(R"(D:\Data\test\a.txt)"), std::string("a.txt"));
    FMT_CHECK_EQ(fmt::file_name_from_source("/tmp/x/y.log"), std::string("y.log"));
    FMT_CHECK_EQ(fmt::file_name_from_source("http://example.com/"), std::string(""));
}

FMT_TEST(File, 扩展名与类型) {
    FMT_CHECK_EQ(fmt::extension_of("test.txt"), std::string(".txt"));
    FMT_CHECK_EQ(fmt::extension_of("PHOTO.JPG"), std::string(".jpg"));  // 统一小写
    FMT_CHECK_EQ(fmt::extension_of("archive.tar.gz"), std::string(".gz"));
    FMT_CHECK_EQ(fmt::extension_of("noext"), std::string(""));
    FMT_CHECK_EQ(fmt::extension_of(".bashrc"), std::string(""));  // 点开头不算扩展名
    FMT_CHECK_EQ(fmt::extension_of("trailing."), std::string(""));
}

FMT_TEST(File, 本地文件暂存) {
    Fixture f;
    const auto prepared = fmt::prepare_upload(*f.paths, fmt::path_to_utf8(f.source), "", kSizeLimit,
                                              nullptr);
    FMT_CHECK(fmt::ok(prepared));
    if (!fmt::ok(prepared)) {
        return;
    }
    const fmt::PreparedUpload& upload = std::get<fmt::PreparedUpload>(prepared);
    FMT_CHECK_EQ(upload.file_name, std::string("source.txt"));
    FMT_CHECK_EQ(upload.size, std::uintmax_t{11});
    FMT_CHECK_EQ(upload.md5, fmt::md5_hex("hello world"));
    FMT_CHECK(fmt::file_exists(upload.temp_path));
    // 临时文件在数据根的 temp/ 下，且带 fmt- 前缀（启动清理认得它）
    FMT_CHECK_EQ(fmt::path_to_utf8(upload.temp_path.parent_path()),
                 fmt::path_to_utf8(f.paths->temp()));
    FMT_CHECK(upload.temp_path.filename().wstring().rfind(L"fmt-upload-", 0) == 0);
}

FMT_TEST(File, 暂存失败会清理临时文件) {
    Fixture f;

    // 本地文件不存在
    const auto missing = fmt::prepare_upload(*f.paths, "D:/definitely/not/here.txt", "",
                                             kSizeLimit, nullptr);
    FMT_CHECK(!fmt::ok(missing));
    FMT_CHECK(fmt::error_of(missing)->code == fmt::ErrorCode::FileNotFound);

    // 只支持 http/https：别的协议直接拒绝（这里不碰网络）
    const auto scheme = fmt::prepare_upload(*f.paths, "ftp://example.com/a.bin", "", kSizeLimit,
                                            nullptr);
    FMT_CHECK(!fmt::ok(scheme));
    FMT_CHECK(fmt::error_of(scheme)->code == fmt::ErrorCode::UrlInvalid);

    // 超过大小上限：临时文件必须被删掉，temp/ 里不留垃圾
    const auto too_large = fmt::prepare_upload(*f.paths, fmt::path_to_utf8(f.source), "",
                                               5 /* 上限 5 字节，源文件 11 字节 */, nullptr);
    FMT_CHECK(!fmt::ok(too_large));
    FMT_CHECK(fmt::error_of(too_large)->code == fmt::ErrorCode::SizeLimitExceeded);

    std::size_t leftovers = 0;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(f.paths->temp(), code)) {
        if (entry.is_regular_file()) {
            ++leftovers;
        }
    }
    FMT_CHECK_EQ(leftovers, std::size_t{0});
}

FMT_TEST(File, 入库写记录并分配file_id) {
    Fixture f;
    const auto first = upload_local(f);
    FMT_CHECK(fmt::ok(first));
    if (!fmt::ok(first)) {
        return;
    }

    const fmt::FileRecord& record = std::get<fmt::FileRecord>(first);
    FMT_CHECK_EQ(record.user, std::string("user"));
    FMT_CHECK_EQ(record.bucket, std::string("工作"));
    FMT_CHECK_EQ(record.file_name, std::string("source.txt"));
    FMT_CHECK_EQ(record.extension, std::string(".txt"));
    FMT_CHECK_EQ(record.file_type, std::string("text"));
    FMT_CHECK_EQ(record.size, std::uintmax_t{11});
    FMT_CHECK_EQ(record.md5, fmt::md5_hex("hello world"));
    FMT_CHECK(!record.is_trash);
    // fmt-YYYYMMDD-0
    FMT_CHECK_EQ(record.file_id.rfind("fmt-" + fmt::local_date_compact() + "-", 0), std::size_t{0});
    FMT_CHECK_EQ(record.file_id.substr(record.file_id.size() - 2), std::string("-0"));

    // 文件真的落到了仓库的日期目录里，临时文件也被消费掉了
    const auto path = fmt::FileService(*f.paths, f.config, nullptr).resolve_path(record);
    FMT_CHECK(fmt::ok(path));
    FMT_CHECK(fmt::file_exists(std::get<std::filesystem::path>(path)));
    FMT_CHECK_EQ(fmt::path_to_utf8(std::get<std::filesystem::path>(path).filename()),
                 std::string("source.txt"));

    // 第二条换名字，序号递增
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(f.source, "second content")));
    const auto second = upload_local(f, "other.txt");
    FMT_CHECK(fmt::ok(second));
    if (fmt::ok(second)) {
        FMT_CHECK_EQ(std::get<fmt::FileRecord>(second).file_id.substr(
                         std::get<fmt::FileRecord>(second).file_id.size() - 2),
                     std::string("-1"));
    }

    // 记录写进了 file.json
    const auto document = fmt::read_json_file(f.paths->file_data());
    FMT_CHECK(fmt::ok(document));
    FMT_CHECK_EQ(std::get<nlohmann::json>(document)["files"].size(), std::size_t{2});
}

FMT_TEST(File, 重复内容与重名都被拒绝) {
    Fixture f;
    FMT_CHECK(fmt::ok(upload_local(f)));

    // 同样的内容换个名字：MD5 去重（第 37 节）
    const auto duplicate = upload_local(f, "copy.txt");
    FMT_CHECK(!fmt::ok(duplicate));
    FMT_CHECK(fmt::error_of(duplicate)->code == fmt::ErrorCode::Md5Duplicate);

    // 不同的内容、同样的名字：文件名冲突（第 38 节），不自动改名
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(f.source, "different content")));
    const auto conflict = upload_local(f, "source.txt");
    FMT_CHECK(!fmt::ok(conflict));
    FMT_CHECK(fmt::error_of(conflict)->code == fmt::ErrorCode::FileNameConflict);
    FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(conflict)->code), 4);

    // 两次失败都不许留下临时文件
    std::size_t leftovers = 0;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(f.paths->temp(), code)) {
        if (entry.is_regular_file()) {
            ++leftovers;
        }
    }
    FMT_CHECK_EQ(leftovers, std::size_t{0});
}

FMT_TEST(File, 没有当前Bucket时拒绝上传) {
    Fixture f;
    f.config.current_bucket.clear();

    fmt::FileService files(*f.paths, f.config, nullptr);
    auto prepared = fmt::prepare_upload(*f.paths, fmt::path_to_utf8(f.source), "", kSizeLimit,
                                        nullptr);
    FMT_CHECK(fmt::ok(prepared));
    if (!fmt::ok(prepared)) {
        return;
    }
    fmt::PreparedUpload upload = std::get<fmt::PreparedUpload>(prepared);
    const auto committed = files.commit_upload(upload);
    FMT_CHECK(!fmt::ok(committed));
    FMT_CHECK(fmt::error_of(committed)->code == fmt::ErrorCode::NoCurrentBucket);
    FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(committed)->code), 3);
}

FMT_TEST(File, 列表与查询) {
    Fixture f;
    FMT_CHECK(fmt::ok(upload_local(f, "a.txt")));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(f.source, "another")));
    FMT_CHECK(fmt::ok(upload_local(f, "b.txt")));

    fmt::FileService files(*f.paths, f.config, nullptr);
    const auto list = files.list();
    FMT_CHECK(fmt::ok(list));
    FMT_CHECK_EQ(std::get<std::vector<fmt::FileRecord>>(list).size(), std::size_t{2});
    FMT_CHECK_EQ(std::get<std::vector<fmt::FileRecord>>(list)[0].file_name, std::string("a.txt"));
    FMT_CHECK_EQ(std::get<std::vector<fmt::FileRecord>>(list)[1].file_name, std::string("b.txt"));

    const std::string id = std::get<std::vector<fmt::FileRecord>>(list)[0].file_id;
    FMT_CHECK(fmt::ok(files.get_by_id(id)));
    FMT_CHECK(fmt::ok(files.get_by_name("b.txt")));

    const auto unknown = files.get_by_id("fmt-20260101-9");
    FMT_CHECK(!fmt::ok(unknown));
    FMT_CHECK(fmt::error_of(unknown)->code == fmt::ErrorCode::FileNotFound);
    const auto unknown_name = files.get_by_name("没有这个文件.txt");
    FMT_CHECK(!fmt::ok(unknown_name));
    FMT_CHECK(fmt::error_of(unknown_name)->code == fmt::ErrorCode::FileNotFound);
}

FMT_TEST(File, 软删除进回收站) {
    Fixture f;
    const auto uploaded = upload_local(f);
    FMT_CHECK(fmt::ok(uploaded));
    if (!fmt::ok(uploaded)) {
        return;
    }
    const fmt::FileRecord record = std::get<fmt::FileRecord>(uploaded);

    fmt::FileService files(*f.paths, f.config, nullptr);
    const auto removed = files.remove(record.file_id);
    FMT_CHECK(fmt::ok(removed));
    if (!fmt::ok(removed)) {
        return;
    }
    const fmt::FileRecord& trashed = std::get<fmt::FileRecord>(removed);
    FMT_CHECK_EQ(trashed.file_id, record.file_id);  // file_id 不变（第 43 节）
    FMT_CHECK(trashed.is_trash);
    FMT_CHECK_EQ(trashed.trash_reason, std::string("file"));

    // 数据搬到了 trash/<user>/.files/<bucket>/YYYY/MM/DD/
    const auto trash_path = files.trash_path_of(trashed);
    FMT_CHECK(fmt::ok(trash_path));
    FMT_CHECK(fmt::file_exists(std::get<std::filesystem::path>(trash_path)));
    FMT_CHECK(fmt::to_forward_slashes(fmt::path_to_utf8(
                  std::get<std::filesystem::path>(trash_path)))
                  .find("trash/user/.files/") != std::string::npos);
    // 仓库里那份已经没了（推出来的路径不存在，兜底也找不到）
    const auto stored_path = files.resolve_path(record);
    FMT_CHECK(!fmt::ok(stored_path));
    FMT_CHECK(fmt::error_of(stored_path)->code == fmt::ErrorCode::FileNotFound);

    // 列表里不再出现；file.json 记了 is_trash + 原因
    const auto list = files.list();
    FMT_CHECK(fmt::ok(list));
    FMT_CHECK_EQ(std::get<std::vector<fmt::FileRecord>>(list).size(), std::size_t{0});

    // trash.json 有一条文件级记录，路径都是相对数据根的
    const auto document = fmt::read_json_file(f.paths->trash_data());
    FMT_CHECK(fmt::ok(document));
    const nlohmann::json& entries = std::get<nlohmann::json>(document)["trash"];
    FMT_CHECK_EQ(entries.size(), std::size_t{1});
    FMT_CHECK_EQ(entries[0].value("file_id", std::string{}), record.file_id);
    FMT_CHECK_EQ(entries[0].value("type", std::string{}), std::string("file"));
    FMT_CHECK_EQ(entries[0].value("original_path", std::string{}).rfind("repository/user/", 0),
                 std::size_t{0});
    FMT_CHECK_EQ(entries[0].value("trash_path", std::string{}).rfind("trash/user/.files/", 0),
                 std::size_t{0});

    // 再删一次：已经在回收站里
    const auto again = files.remove(record.file_id);
    FMT_CHECK(!fmt::ok(again));
    FMT_CHECK(fmt::error_of(again)->code == fmt::ErrorCode::InvalidArgument);
}

FMT_TEST(File, 按文件名也能软删除) {
    Fixture f;
    const auto uploaded = upload_local(f, "doc.txt");
    FMT_CHECK(fmt::ok(uploaded));
    if (!fmt::ok(uploaded)) {
        return;
    }
    const fmt::FileRecord record = std::get<fmt::FileRecord>(uploaded);

    fmt::FileService files(*f.paths, f.config, nullptr);

    // 名字根本不存在 -> FMT-002（不是「参数错误」：确实没这个文件）
    const auto unknown = files.remove("没有这个文件.txt");
    FMT_CHECK(!fmt::ok(unknown));
    FMT_CHECK(fmt::error_of(unknown)->code == fmt::ErrorCode::FileNotFound);
    FMT_CHECK(fmt::error_of(unknown)->message.find("文件不存在") != std::string::npos);

    // 按名字删除与按 id 删除等价
    const auto removed = files.remove("doc.txt");
    FMT_CHECK(fmt::ok(removed));
    if (fmt::ok(removed)) {
        FMT_CHECK_EQ(std::get<fmt::FileRecord>(removed).file_id, record.file_id);
        FMT_CHECK(std::get<fmt::FileRecord>(removed).is_trash);
    }
    const auto trash_path = files.trash_path_of(record);
    FMT_CHECK(fmt::ok(trash_path));
    FMT_CHECK(fmt::file_exists(std::get<std::filesystem::path>(trash_path)));

    // 再按名字删一次：必须说清「已经在回收站里」并给出 file_id，
    // 不能报成「文件不存在」——文件明明还在回收站里。
    const auto again = files.remove("doc.txt");
    FMT_CHECK(!fmt::ok(again));
    FMT_CHECK(fmt::error_of(again)->code == fmt::ErrorCode::InvalidArgument);
    FMT_CHECK(fmt::error_of(again)->message.find("回收站") != std::string::npos);
    FMT_CHECK(fmt::error_of(again)->message.find(record.file_id) != std::string::npos);
}

FMT_TEST(File, 按名字删除用的是记录自己的Bucket) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("生活")));  // 第二个桶（第一个「工作」已是当前）
    FMT_CHECK(fmt::ok(buckets.use("工作")));

    const auto uploaded = upload_local(f, "跨桶.txt");
    FMT_CHECK(fmt::ok(uploaded));
    if (!fmt::ok(uploaded)) {
        return;
    }
    FMT_CHECK_EQ(std::get<fmt::FileRecord>(uploaded).bucket, std::string("工作"));

    // 名字的作用域是「同用户跨 Bucket」，所以切到别的桶也能按名字删到它；
    // 关键是回收站落点要用**记录自己的桶**，不是当前桶。
    FMT_CHECK(fmt::ok(buckets.use("生活")));
    fmt::FileService files(*f.paths, f.config, nullptr);

    const auto removed = files.remove("跨桶.txt");
    FMT_CHECK(fmt::ok(removed));
    if (!fmt::ok(removed)) {
        return;
    }
    const auto trash_path = files.trash_path_of(std::get<fmt::FileRecord>(removed));
    FMT_CHECK(fmt::ok(trash_path));
    const std::string text =
        fmt::to_forward_slashes(fmt::path_to_utf8(std::get<std::filesystem::path>(trash_path)));
    FMT_CHECK(text.find("/工作/") != std::string::npos);
    FMT_CHECK(text.find("/生活/") == std::string::npos);
}

FMT_TEST(File, 大小写不同的同名必须被当成重名) {
    Fixture f;
    const auto first = upload_local(f, "doc.txt");
    FMT_CHECK(fmt::ok(first));
    if (!fmt::ok(first)) {
        return;
    }
    const fmt::FileRecord record = std::get<fmt::FileRecord>(first);

    // 换个内容，用大小写不同的同一个名字再传一次。
    // Windows 的文件名不区分大小写，两者会落到**同一个磁盘路径**上：
    // 要么干净地报重名，要么就会把上一个文件覆盖掉（灾难）。
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(f.source, "different content here")));
    const auto second = upload_local(f, "DOC.TXT");

    // **先查数据完整性**：不管第二次上传成不成功，第一个文件都必须完好。
    // （这条放在断言拒绝之前：万一磁盘被覆盖，先炸的就是它，证据才清楚。）
    fmt::FileService files(*f.paths, f.config, nullptr);
    const auto path = files.resolve_path(record);
    FMT_CHECK(fmt::ok(path));
    if (fmt::ok(path)) {
        const std::filesystem::path stored = std::get<std::filesystem::path>(path);
        FMT_CHECK(fmt::file_exists(stored));
        FMT_CHECK_EQ(std::filesystem::file_size(stored), record.size);
        const auto text = fmt::read_text_file(stored);
        FMT_CHECK(fmt::ok(text));
        FMT_CHECK_EQ(std::get<std::string>(text), std::string("hello world"));
    }

    FMT_CHECK(!fmt::ok(second));
}

FMT_TEST(File, 按名字查询不区分大小写) {
    Fixture f;
    FMT_CHECK(fmt::ok(upload_local(f, "Report.txt")));

    fmt::FileService files(*f.paths, f.config, nullptr);
    // Windows 的文件名不区分大小写，用户敲大写也要能找到
    const auto lower = files.get_by_name("report.txt");
    FMT_CHECK(fmt::ok(lower));
    if (fmt::ok(lower)) {
        FMT_CHECK_EQ(std::get<fmt::FileRecord>(lower).file_name, std::string("Report.txt"));
    }
    FMT_CHECK(fmt::ok(files.remove("REPORT.TXT")));
}

FMT_TEST(File, 与file_id同形的名字不能上传) {
    Fixture f;

    // 显式给的名字
    const auto explicit_name = fmt::prepare_upload(*f.paths, fmt::path_to_utf8(f.source),
                                                   "fmt-20261008-0", kSizeLimit, nullptr);
    FMT_CHECK(!fmt::ok(explicit_name));
    FMT_CHECK(fmt::error_of(explicit_name)->code == fmt::ErrorCode::FileNameLikeFileId);

    // 从来源推断出来的名字走的是同一道校验
    const std::filesystem::path tricky = f.temp / "fmt-20261008-0";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(tricky, "x")));
    const auto derived =
        fmt::prepare_upload(*f.paths, fmt::path_to_utf8(tricky), "", kSizeLimit, nullptr);
    FMT_CHECK(!fmt::ok(derived));
    FMT_CHECK(fmt::error_of(derived)->code == fmt::ErrorCode::FileNameLikeFileId);
}

FMT_TEST(File, 标识与名字同时命中时报歧义) {
    Fixture f;

    // 手工造一段「旧数据」：一个文件的 file_id 恰好等于另一个文件的名字。
    // 这种名字现在上传时会被 FMT-106 拒绝，只可能来自旧版本或手工编辑过的 file.json。
    nlohmann::json document = fmt::make_collection(1, "files");
    const auto add = [&document](const std::string& id, const std::string& name) {
        nlohmann::json item = nlohmann::json::object();
        item["file_id"] = id;
        item["user"] = "user";
        item["bucket"] = "工作";
        item["file_name"] = name;
        item["extension"] = ".txt";
        item["file_type"] = "text";
        item["size"] = 1;
        item["md5"] = "d41d8cd98f00b204e9800998ecf8427e";
        item["is_trash"] = false;
        item["trash_reason"] = "";
        document["files"].push_back(std::move(item));
    };
    add("fmt-20261008-0", "a.txt");
    add("fmt-20261008-1", "fmt-20261008-0");
    FMT_CHECK(fmt::ok(fmt::write_json_file(f.paths->file_data(), document)));

    // 不能猜：明确报歧义，并让用户用 file_id 指定
    fmt::FileService files(*f.paths, f.config, nullptr);
    const auto ambiguous = files.remove("fmt-20261008-0");
    FMT_CHECK(!fmt::ok(ambiguous));
    FMT_CHECK(fmt::error_of(ambiguous)->code == fmt::ErrorCode::InvalidArgument);
    FMT_CHECK(fmt::error_of(ambiguous)->message.find("歧义") != std::string::npos);
    FMT_CHECK(fmt::error_of(ambiguous)->message.find("fmt-20261008-1") != std::string::npos);

    // 预检要把**两条候选**都摊出来（y/N 解决不了歧义，只能让用户改用 file_id）
    const auto checked = files.check_remove("fmt-20261008-0");
    FMT_CHECK(fmt::ok(checked));
    if (fmt::ok(checked)) {
        const fmt::FileDeleteCheck& check = std::get<fmt::FileDeleteCheck>(checked);
        FMT_CHECK(check.ambiguous);
        FMT_CHECK(!check.other_bucket);
        FMT_CHECK_EQ(check.candidates.size(), std::size_t{2});
        FMT_CHECK(check.message.find("file_id") != std::string::npos);
    }

    // 不歧义的名字照常
    FMT_CHECK(!fmt::ok(files.remove("a.txt")) ||
              fmt::error_of(files.remove("a.txt"))->code == fmt::ErrorCode::FileNotFound);
}

FMT_TEST(File, 删除预检会把情况说清楚) {
    Fixture f;
    fmt::BucketService buckets(*f.paths, f.config, nullptr);
    FMT_CHECK(fmt::ok(buckets.create("生活")));
    FMT_CHECK(fmt::ok(buckets.use("工作")));

    const auto uploaded = upload_local(f, "a.txt");
    FMT_CHECK(fmt::ok(uploaded));
    if (!fmt::ok(uploaded)) {
        return;
    }

    fmt::FileService files(*f.paths, f.config, nullptr);

    // 同一个桶：没什么要先说清楚的
    const auto same = files.check_remove("a.txt");
    FMT_CHECK(fmt::ok(same));
    if (fmt::ok(same)) {
        const fmt::FileDeleteCheck& check = std::get<fmt::FileDeleteCheck>(same);
        FMT_CHECK(!check.other_bucket);
        FMT_CHECK(!check.ambiguous);
        FMT_CHECK_EQ(check.bucket, std::string("工作"));
        FMT_CHECK_EQ(check.current_bucket, std::string("工作"));
        FMT_CHECK(check.message.empty());
        FMT_CHECK(!check.path.empty());
    }

    // 切到另一个桶：预检要能说清「属于哪个桶、当前是哪个桶」
    FMT_CHECK(fmt::ok(buckets.use("生活")));
    const auto cross = files.check_remove("a.txt");
    FMT_CHECK(fmt::ok(cross));
    if (fmt::ok(cross)) {
        const fmt::FileDeleteCheck& check = std::get<fmt::FileDeleteCheck>(cross);
        FMT_CHECK(check.other_bucket);
        FMT_CHECK(!check.ambiguous);
        FMT_CHECK_EQ(check.bucket, std::string("工作"));
        FMT_CHECK_EQ(check.current_bucket, std::string("生活"));
        FMT_CHECK(check.message.find("工作") != std::string::npos);
        FMT_CHECK(check.message.find("生活") != std::string::npos);
    }

    // 预检**不改任何东西**：文件还在仓库里
    const auto path = files.resolve_path(std::get<fmt::FileRecord>(uploaded));
    FMT_CHECK(fmt::ok(path));
    FMT_CHECK(fmt::file_exists(std::get<std::filesystem::path>(path)));
}

FMT_TEST(File, 从HTTP下载入库) {
    Fixture f;

    const std::string payload = "0123456789abcdef";
    httplib::Server server;
    server.Get("/hello.bin", [&payload](const httplib::Request&, httplib::Response& response) {
        response.set_content(payload, "application/octet-stream");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    FMT_CHECK(port > 0);
    std::thread worker([&server] { server.listen_after_bind(); });
    server.wait_until_ready();

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/hello.bin";
    const auto prepared = fmt::prepare_upload(*f.paths, url, "", kSizeLimit, nullptr);
    FMT_CHECK(fmt::ok(prepared));
    if (fmt::ok(prepared)) {
        const fmt::PreparedUpload& upload = std::get<fmt::PreparedUpload>(prepared);
        FMT_CHECK_EQ(upload.file_name, std::string("hello.bin"));
        FMT_CHECK_EQ(upload.size, std::uintmax_t{16});
        FMT_CHECK_EQ(upload.md5, fmt::md5_hex(payload));

        fmt::PreparedUpload ready = upload;
        fmt::FileService files(*f.paths, f.config, nullptr);
        const auto committed = files.commit_upload(ready);
        FMT_CHECK(fmt::ok(committed));
        if (fmt::ok(committed)) {
            FMT_CHECK_EQ(std::get<fmt::FileRecord>(committed).file_type, std::string("other"));
        }
    }

    // 404 -> 下载失败，且不留临时文件
    const auto missing = fmt::prepare_upload(
        *f.paths, "http://127.0.0.1:" + std::to_string(port) + "/nope.bin", "", kSizeLimit, nullptr);
    FMT_CHECK(!fmt::ok(missing));
    FMT_CHECK(fmt::error_of(missing)->code == fmt::ErrorCode::DownloadFailed);

    std::size_t leftovers = 0;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(f.paths->temp(), code)) {
        if (entry.is_regular_file()) {
            ++leftovers;
        }
    }
    FMT_CHECK_EQ(leftovers, std::size_t{0});

    server.stop();
    worker.join();
}
