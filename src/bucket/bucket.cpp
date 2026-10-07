#include "fmt/bucket/bucket.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"
#include "fmt/common/validation.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt {
namespace {

// "2026-10-08 01:23:45" -> "20261008012345"：回收站目录名的时间戳部分。
std::string compact_stamp() {
    std::string stamp = local_timestamp();
    stamp.erase(std::remove_if(stamp.begin(), stamp.end(),
                               [](char ch) { return ch == '-' || ch == ':' || ch == ' '; }),
                stamp.end());
    return stamp;
}

// 文件记录里「为什么在回收站」的标记：桶被删 vs 文件自己删过。
// 回退桶时只翻回前者，不然会把用户单独删过的文件一起放出来。
constexpr const char* kTrashReasonBucket = "bucket";

bool is_dot_entry(const std::string& name) { return !name.empty() && name.front() == '.'; }

bool is_digits(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

// 目录名是否符合桶级条目的形状：`<名字>_<14 位时间戳>` 或 `<名字>_<时间戳>_<序号>`。
//
// 文件级条目已经被收进 `trash/<user>/.files/`（点开头，扫描时跳过），
// 这里再加一道形状检查是第二道保险：手工拷进来的、别的东西都不会被误当成桶级条目。
bool looks_like_trashed_bucket(const std::string& name) {
    const std::size_t last = name.rfind('_');
    if (last == std::string::npos || last == 0) {
        return false;
    }
    if (is_digits(name.substr(last + 1)) && name.size() - last - 1 == 14) {
        return true;
    }

    // <名字>_<14 位时间戳>_<序号 1-3 位>
    const std::size_t previous = name.rfind('_', last - 1);
    if (previous == std::string::npos || previous == 0) {
        return false;
    }
    const std::string stamp = name.substr(previous + 1, last - previous - 1);
    const std::string suffix = name.substr(last + 1);
    return stamp.size() == 14 && is_digits(stamp) && suffix.size() <= 3 && is_digits(suffix);
}

}  // namespace

BucketService::BucketService(const PathManager& paths, Config& config, Logger* logger)
    : paths_(paths), config_(config), logger_(logger) {}

std::filesystem::path BucketService::user_root() const {
    return paths_.repository() / path_from_utf8(config_.current_user);
}

std::filesystem::path BucketService::bucket_path(std::string_view name) const {
    return user_root() / path_from_utf8(std::string(name));
}

std::filesystem::path BucketService::trash_user_root() const {
    return paths_.trash() / path_from_utf8(config_.current_user);
}

std::filesystem::path BucketService::original_index_path() const {
    return trash_user_root() / path_from_utf8(kOriginalIndexName);
}

std::filesystem::path BucketService::directory_of(std::string_view name) const {
    return bucket_path(name);
}

Status BucketService::require_current_user() const {
    if (config_.current_user.empty()) {
        return make_error(ErrorCode::NoCurrentUser, "未设置当前用户");
    }
    return std::monostate{};
}

Status BucketService::persist_config() { return save_config(paths_, config_); }

// ---------------------------------------------------------------------------
// create / list / get / use
// ---------------------------------------------------------------------------

Status BucketService::create(std::string_view name) {
    if (const Status status = require_current_user(); !ok(status)) {
        return status;
    }
    if (const Status status = validate_bucket_name(name); !ok(status)) {
        return status;
    }

    const std::filesystem::path directory = bucket_path(name);
    if (directory_exists(directory)) {
        return make_error(ErrorCode::BucketAlreadyExists, "Bucket 已存在：" + std::string(name));
    }

    if (const Status status = ensure_directory(user_root()); !ok(status)) {
        return status;
    }
    if (const Status status = ensure_directory(directory); !ok(status)) {
        return status;
    }

    // 第一个 Bucket 自动成为当前 Bucket（第 28 节）。
    if (config_.current_bucket.empty()) {
        config_.current_bucket = std::string(name);
        if (const Status status = persist_config(); !ok(status)) {
            return status;
        }
    }

    if (logger_ != nullptr) {
        logger_->info("Bucket", "创建 Bucket：" + std::string(name));
    }
    return std::monostate{};
}

Result<std::vector<BucketInfo>> BucketService::list() {
    if (const Status status = require_current_user(); !ok(status)) {
        return *error_of(status);
    }

    std::vector<BucketInfo> buckets;
    const std::filesystem::path root = user_root();

    std::error_code code;
    if (std::filesystem::is_directory(root, code)) {
        for (const auto& entry : std::filesystem::directory_iterator(root, code)) {
            if (code) {
                break;
            }
            std::error_code type_code;
            if (!entry.is_directory(type_code)) {
                continue;
            }
            BucketInfo info;
            info.name = path_to_utf8(entry.path().filename());
            // 目录名不区分大小写（Windows）：current_bucket 存的是用户敲的拼写，
            // 可能和磁盘上的实际名字大小写不同，比较时不能按字节精确比。
            info.is_current = iequals(info.name, config_.current_bucket);
            buckets.push_back(std::move(info));
        }
    }

    std::sort(buckets.begin(), buckets.end(),
              [](const BucketInfo& left, const BucketInfo& right) { return left.name < right.name; });
    return buckets;
}

Result<BucketInfo> BucketService::get(std::string_view name) {
    if (const Status status = require_current_user(); !ok(status)) {
        return *error_of(status);
    }
    if (name.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少 Bucket 名称");
    }

    const std::filesystem::path directory = bucket_path(name);
    if (!directory_exists(directory)) {
        return make_error(ErrorCode::BucketNotFound, "Bucket 不存在：" + std::string(name));
    }

    BucketInfo info;
    info.name = std::string(name);
    info.is_current = (info.name == config_.current_bucket);
    return info;
}

Status BucketService::use(std::string_view name) {
    if (const Status status = require_current_user(); !ok(status)) {
        return status;
    }
    if (name.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少 Bucket 名称");
    }

    if (!directory_exists(bucket_path(name))) {
        return make_error(ErrorCode::BucketNotFound, "Bucket 不存在：" + std::string(name));
    }

    config_.current_bucket = std::string(name);
    if (const Status status = persist_config(); !ok(status)) {
        return status;
    }

    if (logger_ != nullptr) {
        logger_->info("Bucket", "切换当前 Bucket：" + std::string(name));
    }
    return std::monostate{};
}

// ---------------------------------------------------------------------------
// delete（移入回收站）
// ---------------------------------------------------------------------------

Result<BucketRemoval> BucketService::remove(std::string_view name) {
    if (const Status status = require_current_user(); !ok(status)) {
        return *error_of(status);
    }
    if (name.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少 Bucket 名称");
    }
    if (!directory_exists(bucket_path(name))) {
        return make_error(ErrorCode::BucketNotFound, "Bucket 不存在：" + std::string(name));
    }

    // 先读索引：它坏了就不能继续——否则桶搬进回收站却没有身份记录，原名就丢了。
    Result<std::vector<TrashBucket>> loaded = load_original_index();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    std::vector<TrashBucket> entries = std::get<std::vector<TrashBucket>>(loaded);

    BucketRemoval removal;
    removal.was_current = iequals(config_.current_bucket, name);
    removal.trashed_name = unique_trashed_name(name);

    if (const Status status = move_bucket_to_trash(name, removal.trashed_name, &removal.moved_to);
        !ok(status)) {
        return *error_of(status);
    }

    TrashBucket record;
    record.trashed_name = removal.trashed_name;
    record.original_name = std::string(name);
    record.deleted_at = local_datetime_iso();

    entries.push_back(record);
    if (const Status status = save_original_index(entries); !ok(status)) {
        // 身份记录写不进去就把目录搬回去：宁可删不掉，也不要留下一个「不知道原名」的条目。
        std::error_code ignored;
        std::filesystem::rename(removal.moved_to, bucket_path(name), ignored);
        return *error_of(status);
    }

    if (const Status status = set_bucket_files_trash_flag(name, true, &removal.files_affected);
        !ok(status)) {
        return *error_of(status);
    }

    if (removal.was_current) {
        config_.current_bucket.clear();  // 不自动切换到别的 Bucket（第 30 节）
        if (const Status status = persist_config(); !ok(status)) {
            return *error_of(status);
        }
    }

    if (logger_ != nullptr) {
        logger_->info("Bucket", "删除 Bucket：" + std::string(name) + " -> trash/" +
                                    config_.current_user + "/" + removal.trashed_name + "（" +
                                    std::to_string(removal.files_affected) + " 个文件标记为已删除）");
    }
    return removal;
}

Status BucketService::move_bucket_to_trash(std::string_view name, const std::string& trashed_name,
                                           std::filesystem::path* moved_to) {
    const std::filesystem::path source = bucket_path(name);
    const std::filesystem::path destination = trash_user_root() / path_from_utf8(trashed_name);

    if (const Status status = ensure_directory(trash_user_root()); !ok(status)) {
        return status;
    }

    std::error_code code;
    std::filesystem::rename(source, destination, code);
    if (code) {
        return make_error(ErrorCode::StorageError,
                          "移动 Bucket 到回收站失败：" + path_to_utf8(source) + " -> " +
                              path_to_utf8(destination) + "（" + code.message() + "）");
    }

    if (moved_to != nullptr) {
        *moved_to = destination;
    }
    return std::monostate{};
}

std::string BucketService::unique_trashed_name(std::string_view name) const {
    const std::string base = std::string(name) + "_" + compact_stamp();

    // 同一秒内删两次（或目录恰好同名）时加序号，绝不覆盖已有条目。
    std::string candidate = base;
    for (int suffix = 2; path_exists(trash_user_root() / path_from_utf8(candidate)); ++suffix) {
        candidate = base + "_" + std::to_string(suffix);
    }
    return candidate;
}

// ---------------------------------------------------------------------------
// 回收站：索引（trash/<user>/.original 是桶级记录的权威）
// ---------------------------------------------------------------------------

Result<std::vector<TrashBucket>> BucketService::load_original_index() const {
    std::vector<TrashBucket> entries;

    const std::filesystem::path index = original_index_path();
    if (!file_exists(index)) {
        return entries;
    }

    Result<nlohmann::json> parsed = read_json_file(index);
    if (!ok(parsed)) {
        return *error_of(parsed);  // 损坏就报错，绝不重置（索引丢了原名就找不回来了）
    }
    nlohmann::json& document = std::get<nlohmann::json>(parsed);

    if (const Status version = check_version(document, 1); !ok(version)) {
        return *error_of(version);
    }
    const auto list = document.find("buckets");
    if (list == document.end() || !list->is_array()) {
        return make_error(ErrorCode::JsonParseError, ".original 缺少 buckets 数组");
    }

    for (const nlohmann::json& item : *list) {
        if (!item.is_object()) {
            continue;
        }
        TrashBucket entry;
        entry.trashed_name = item.value("trashed", std::string{});
        entry.original_name = item.value("original", std::string{});
        entry.deleted_at = item.value("deleted_at", std::string{});
        if (!entry.trashed_name.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

Status BucketService::save_original_index(const std::vector<TrashBucket>& entries) const {
    nlohmann::json document = nlohmann::json::object();
    document["version"] = 1;
    document["buckets"] = nlohmann::json::array();

    for (const TrashBucket& entry : entries) {
        nlohmann::json item = nlohmann::json::object();
        item["trashed"] = entry.trashed_name;
        item["original"] = entry.original_name;
        item["deleted_at"] = entry.deleted_at;
        document["buckets"].push_back(std::move(item));
    }

    if (const Status status = ensure_directory(trash_user_root()); !ok(status)) {
        return status;
    }
    return write_json_file(original_index_path(), document);
}

Result<std::vector<TrashBucket>> BucketService::list_trashed() {
    if (const Status status = require_current_user(); !ok(status)) {
        return *error_of(status);
    }

    Result<std::vector<TrashBucket>> loaded = load_original_index();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    std::vector<TrashBucket> entries = std::get<std::vector<TrashBucket>>(loaded);

    const std::filesystem::path root = trash_user_root();

    // 索引里有、目录没了的：如实标出来，不擅自清理（数据一致性问题只报告）。
    for (TrashBucket& entry : entries) {
        entry.directory_present = directory_exists(root / path_from_utf8(entry.trashed_name));
    }

    // 目录里有、索引里没有的（手工拷进来的、旧版本留下的）：也列出来，
    // 但原名为空——回退会被拒绝，因为「剥掉时间戳猜原名」是不可靠的。
    std::error_code code;
    if (std::filesystem::is_directory(root, code)) {
        for (const auto& item : std::filesystem::directory_iterator(root, code)) {
            if (code) {
                break;
            }
            std::error_code type_code;
            if (!item.is_directory(type_code)) {
                continue;
            }
            const std::string name = path_to_utf8(item.path().filename());
            if (is_dot_entry(name) || !looks_like_trashed_bucket(name)) {
                continue;  // .files/ 之类的点目录、以及不像桶级条目的东西都跳过
            }
            const bool known =
                std::any_of(entries.begin(), entries.end(), [&name](const TrashBucket& entry) {
                    return entry.trashed_name == name;
                });
            if (!known) {
                TrashBucket entry;
                entry.trashed_name = name;
                entries.push_back(std::move(entry));
            }
        }
    }

    std::sort(entries.begin(), entries.end(),
              [](const TrashBucket& left, const TrashBucket& right) {
                  return left.trashed_name < right.trashed_name;
              });
    return entries;
}

// ---------------------------------------------------------------------------
// 回退（整单判定，不做部分恢复）
// ---------------------------------------------------------------------------

Result<TrashBucket> BucketService::restore(std::string_view identifier) {
    if (const Status status = require_current_user(); !ok(status)) {
        return *error_of(status);
    }
    if (identifier.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少回收站条目的名称");
    }

    Result<std::vector<TrashBucket>> loaded = load_original_index();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    const std::vector<TrashBucket> entries = std::get<std::vector<TrashBucket>>(loaded);

    // 先用回收站里的名字找；找不到再按原桶名找，但必须唯一。
    Result<TrashLookup> lookup = find_trashed(entries, identifier);
    if (!ok(lookup)) {
        return *error_of(lookup);
    }
    const TrashLookup found = std::get<TrashLookup>(lookup);

    if (!found.found) {
        // 目录在回收站里、但索引里没有它（手工拷进来的、旧版本留下的）：
        // 原桶名未知，猜名字不可靠，明确拒绝而不是随便找个位置放回去。
        if (directory_exists(trash_user_root() / path_from_utf8(std::string(identifier)))) {
            return make_error(ErrorCode::InvalidArgument,
                              "回收站条目缺少原桶名记录（" + std::string(kOriginalIndexName) +
                                  "），无法回退：" + std::string(identifier));
        }
        return make_error(ErrorCode::TrashEntryNotFound,
                          "回收站里没有这个 Bucket：" + std::string(identifier));
    }

    const TrashBucket entry = entries[found.index];
    if (entry.original_name.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "回收站条目缺少原桶名记录（" + std::string(kOriginalIndexName) +
                              "），无法回退：" + entry.trashed_name);
    }

    const std::filesystem::path source = trash_user_root() / path_from_utf8(entry.trashed_name);
    if (!directory_exists(source)) {
        return make_error(ErrorCode::TrashEntryNotFound,
                          "回收站目录已不存在：" + entry.trashed_name);
    }

    const std::filesystem::path target = bucket_path(entry.original_name);
    if (path_exists(target)) {
        // 用户定的规则：目标已存在就整单拒绝。不覆盖、不改名、不做部分恢复。
        return make_error(ErrorCode::RestoreConflict,
                          "回退失败：Bucket 已存在：" + entry.original_name);
    }

    if (const Status status = ensure_directory(user_root()); !ok(status)) {
        return *error_of(status);
    }

    std::error_code code;
    std::filesystem::rename(source, target, code);
    if (code) {
        return make_error(ErrorCode::StorageError, "从回收站回退失败：" + path_to_utf8(source) +
                                                       " -> " + path_to_utf8(target) + "（" +
                                                       code.message() + "）");
    }

    // 索引先减掉这一条：写不进去就把目录退回去，别出现「目录已回退、索引还在」。
    std::vector<TrashBucket> remaining;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i != found.index) {
            remaining.push_back(entries[i]);
        }
    }
    if (const Status status = save_original_index(remaining); !ok(status)) {
        std::error_code ignored;
        std::filesystem::rename(target, source, ignored);
        return *error_of(status);
    }

    std::size_t affected = 0;
    if (const Status status = set_bucket_files_trash_flag(entry.original_name, false, &affected);
        !ok(status)) {
        return *error_of(status);
    }

    if (logger_ != nullptr) {
        logger_->info("Bucket", "回退 Bucket：" + entry.trashed_name + " -> " +
                                    entry.original_name + "（" + std::to_string(affected) +
                                    " 个文件恢复为正常）");
    }
    TrashBucket restored = entry;
    restored.directory_present = true;
    return restored;
}

// ---------------------------------------------------------------------------
// 定位 / 详情 / 永久删除
// ---------------------------------------------------------------------------

Result<BucketService::TrashLookup> BucketService::find_trashed(
    const std::vector<TrashBucket>& entries, std::string_view identifier) const {
    TrashLookup lookup;

    // 回收站里的名字优先：它是精确的、不带歧义的。
    // 比较不区分大小写（桶名那部分可能是用户按不同大小写敲的）。
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (iequals(entries[i].trashed_name, identifier)) {
            lookup.found = true;
            lookup.index = i;
            return lookup;
        }
    }

    // 再用原桶名：同名多条时必须让调用方改用回收站里的名字。
    std::vector<std::size_t> matched;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].original_name.empty() && iequals(entries[i].original_name, identifier)) {
            matched.push_back(i);
        }
    }
    if (matched.size() > 1) {
        std::string candidates;
        for (const std::size_t index : matched) {
            candidates += (candidates.empty() ? "" : "、") + entries[index].trashed_name;
        }
        return make_error(ErrorCode::InvalidArgument,
                          "有多个同名 Bucket 被删除，请用回收站里的名字指定：" + candidates);
    }
    if (matched.size() == 1) {
        lookup.found = true;
        lookup.index = matched.front();
    }
    return lookup;
}

Result<TrashBucketDetail> BucketService::get_trashed(std::string_view identifier) {
    if (const Status status = require_current_user(); !ok(status)) {
        return *error_of(status);
    }
    if (identifier.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少回收站条目的名称");
    }

    Result<std::vector<TrashBucket>> loaded = load_original_index();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    const std::vector<TrashBucket> entries = std::get<std::vector<TrashBucket>>(loaded);

    Result<TrashLookup> lookup = find_trashed(entries, identifier);
    if (!ok(lookup)) {
        return *error_of(lookup);
    }
    const TrashLookup found = std::get<TrashLookup>(lookup);

    TrashBucketDetail detail;
    if (found.found) {
        detail.bucket = entries[found.index];
    } else {
        detail.bucket.trashed_name = std::string(identifier);  // 孤儿目录：按目录名查
    }

    detail.directory = trash_user_root() / path_from_utf8(detail.bucket.trashed_name);
    detail.bucket.directory_present = directory_exists(detail.directory);
    if (!detail.bucket.directory_present) {
        if (!found.found) {
            return make_error(ErrorCode::TrashEntryNotFound,
                              "回收站里没有这个条目：" + std::string(identifier));
        }
        return detail;  // 索引里有、目录没了：如实报告 present = false
    }

    // 单条查询，走一遍目录是可接受的成本（列表查询不做这件事）。
    std::error_code code;
    for (const auto& item :
         std::filesystem::recursive_directory_iterator(detail.directory, code)) {
        std::error_code type_code;
        if (item.is_regular_file(type_code)) {
            ++detail.file_count;
            detail.byte_count += item.file_size(type_code);
        }
    }
    return detail;
}

Result<TrashPurge> BucketService::purge(std::string_view identifier) {
    if (const Status status = require_current_user(); !ok(status)) {
        return *error_of(status);
    }
    if (identifier.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少回收站条目的名称");
    }

    Result<std::vector<TrashBucket>> loaded = load_original_index();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    const std::vector<TrashBucket> entries = std::get<std::vector<TrashBucket>>(loaded);

    Result<TrashLookup> lookup = find_trashed(entries, identifier);
    if (!ok(lookup)) {
        return *error_of(lookup);
    }
    const TrashLookup found = std::get<TrashLookup>(lookup);

    TrashPurge result;
    if (found.found) {
        result.trashed_name = entries[found.index].trashed_name;
        result.original_name = entries[found.index].original_name;
    } else {
        result.trashed_name = std::string(identifier);
    }

    const std::filesystem::path directory =
        trash_user_root() / path_from_utf8(result.trashed_name);
    const bool present = directory_exists(directory);
    if (!present && !found.found) {
        return make_error(ErrorCode::TrashEntryNotFound,
                          "回收站里没有这个条目：" + std::string(identifier));
    }

    // ① 先删磁盘数据。失败就什么都没变（索引还在，可以再来一次）。
    if (present) {
        std::error_code code;
        for (const auto& item : std::filesystem::recursive_directory_iterator(directory, code)) {
            std::error_code type_code;
            if (item.is_regular_file(type_code)) {
                ++result.removed_files;
            }
        }
        code.clear();
        std::filesystem::remove_all(directory, code);
        if (code) {
            return make_error(ErrorCode::StorageError, "永久删除失败：" + path_to_utf8(directory) +
                                                           "（" + code.message() + "）");
        }
    }

    // ② 再清 metadata：只清「因桶被删」的记录，文件自己删过的不动。
    if (!result.original_name.empty()) {
        if (const Status status =
                remove_bucket_file_records(result.original_name, &result.removed_records);
            !ok(status)) {
            return *error_of(status);
        }
    }

    // ③ 最后摘索引：顺序刻意如此，中途任何失败都能重来。
    if (found.found) {
        std::vector<TrashBucket> remaining;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i != found.index) {
                remaining.push_back(entries[i]);
            }
        }
        if (const Status status = save_original_index(remaining); !ok(status)) {
            return *error_of(status);
        }
    }

    if (logger_ != nullptr) {
        logger_->info("Trash", "永久删除回收站条目：" + result.trashed_name + "（" +
                                   std::to_string(result.removed_files) + " 个文件、" +
                                   std::to_string(result.removed_records) + " 条记录）");
    }
    return result;
}

// ---------------------------------------------------------------------------
// 文件记录的 is_trash 维护
// ---------------------------------------------------------------------------

Status BucketService::set_bucket_files_trash_flag(std::string_view name, bool trashed,
                                                  std::size_t* affected) {
    if (affected != nullptr) {
        *affected = 0;
    }
    if (!file_exists(paths_.file_data())) {
        return std::monostate{};  // 还没有 file.json：没有文件记录要改
    }

    Result<nlohmann::json> parsed = read_json_file(paths_.file_data());
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    nlohmann::json& document = std::get<nlohmann::json>(parsed);

    if (const Status version = check_version(document, 1); !ok(version)) {
        return version;
    }
    const auto records = document.find("files");
    if (records == document.end() || !records->is_array()) {
        return make_error(ErrorCode::JsonParseError, "file.json 缺少 files 数组");
    }

    std::size_t changed = 0;
    for (nlohmann::json& record : *records) {
        if (!record.is_object()) {
            continue;
        }
        if (record.value("user", std::string{}) != config_.current_user) {
            continue;
        }
        if (record.value("bucket", std::string{}) != name) {
            continue;
        }

        const std::string reason = record.value("trash_reason", std::string{});
        if (trashed) {
            if (record.value("is_trash", false)) {
                continue;  // 已经在回收站里（文件自己删过），不重复计数
            }
            record["is_trash"] = true;
            record["trash_reason"] = kTrashReasonBucket;
            ++changed;
            continue;
        }

        // 回退：只放回「因为桶被删」而进回收站的文件。用户单独删过的保持不动。
        if (!record.value("is_trash", false) || reason != kTrashReasonBucket) {
            continue;
        }
        record["is_trash"] = false;
        record["trash_reason"] = "";
        ++changed;
    }

    if (changed > 0) {
        if (const Status status = write_json_file(paths_.file_data(), document); !ok(status)) {
            return status;
        }
    }
    if (affected != nullptr) {
        *affected = changed;
    }
    return std::monostate{};
}

Status BucketService::remove_bucket_file_records(std::string_view bucket_name,
                                                 std::size_t* removed) {
    if (removed != nullptr) {
        *removed = 0;
    }
    if (!file_exists(paths_.file_data())) {
        return std::monostate{};
    }

    Result<nlohmann::json> parsed = read_json_file(paths_.file_data());
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    nlohmann::json& document = std::get<nlohmann::json>(parsed);

    if (const Status version = check_version(document, 1); !ok(version)) {
        return version;
    }
    const auto records = document.find("files");
    if (records == document.end() || !records->is_array()) {
        return make_error(ErrorCode::JsonParseError, "file.json 缺少 files 数组");
    }

    nlohmann::json kept = nlohmann::json::array();
    std::size_t dropped = 0;
    for (const nlohmann::json& record : *records) {
        if (record.is_object() && record.value("user", std::string{}) == config_.current_user &&
            record.value("bucket", std::string{}) == bucket_name &&
            record.value("is_trash", false) &&
            record.value("trash_reason", std::string{}) == kTrashReasonBucket) {
            ++dropped;  // 它的数据就在刚删掉的那个回收站目录里
            continue;
        }
        kept.push_back(record);
    }

    if (dropped > 0) {
        document["files"] = std::move(kept);
        if (const Status status = write_json_file(paths_.file_data(), document); !ok(status)) {
            return status;
        }
    }
    if (removed != nullptr) {
        *removed = dropped;
    }
    return std::monostate{};
}
// ---------------------------------------------------------------------------
// 当前 Bucket
// ---------------------------------------------------------------------------

Status BucketService::refresh_current_bucket() {
    if (config_.current_bucket.empty()) {
        return std::monostate{};
    }
    if (directory_exists(bucket_path(config_.current_bucket))) {
        return std::monostate{};
    }

    if (logger_ != nullptr) {
        logger_->warn("Bucket", "当前 Bucket 已不存在，置空：" + config_.current_bucket);
    }
    config_.current_bucket.clear();
    return persist_config();
}

}  // namespace fmt
