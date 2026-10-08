#include "fmt/trash/trash.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

#include "fmt/bucket/bucket.hpp"
#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/file/file.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt {
namespace {

constexpr const char* kTypeFile = "file";
constexpr const char* kTypeBucket = "bucket";

// 桶级条目 -> 统一视图
TrashEntry to_entry(const TrashBucket& bucket) {
    TrashEntry entry;
    entry.type = kTypeBucket;
    entry.id = bucket.trashed_name;
    entry.name = bucket.original_name.empty() ? bucket.trashed_name : bucket.original_name;
    entry.deleted_at = bucket.deleted_at;
    entry.files = 0;  // 由调用方按需补（要遍历目录，列表里不做）
    entry.present = bucket.directory_present;
    entry.restorable = !bucket.original_name.empty();  // 没身份记录就没法知道放回哪儿
    if (!entry.restorable) {
        entry.message = "回收站条目缺少原桶名记录，无法回退";
    }
    return entry;
}

}  // namespace

TrashService::TrashService(const PathManager& paths, Config& config, Logger* logger)
    : paths_(paths), config_(config), logger_(logger) {}

Result<TrashEntry> TrashService::entry_of_file(std::string_view file_id) const {
    FileService files(paths_, config_, logger_);
    Result<FileRecord> found = files.get_by_id(file_id);
    if (!ok(found)) {
        return *error_of(found);
    }
    const FileRecord& record = std::get<FileRecord>(found);

    TrashEntry entry;
    entry.type = kTypeFile;
    entry.id = record.file_id;
    entry.name = record.file_name;
    entry.bucket = record.bucket;
    entry.deleted_at = record.deleted_at;
    entry.bytes = record.size;
    entry.files = 1;
    entry.present = true;
    // 随桶一起删除的文件：整棵树挂在桶级条目下，**只能整体恢复桶**（第 58 节）
    entry.restorable = (record.trash_reason == "file");
    if (entry.restorable) {
        const Result<std::filesystem::path> path = files.trash_path_of(record);
        if (ok(path)) {
            const std::filesystem::path trashed = std::get<std::filesystem::path>(path);
            entry.trash_path = relative_path_text(paths_.root(), trashed);
            entry.present = file_exists(trashed);
            if (!entry.present) {
                entry.message = "回收站里找不到文件数据：" + entry.trash_path;
            }
        }
        return entry;
    }

    // 随桶一起删除的：数据在**桶的**回收站目录里（trash/<用户>/<桶>_<时间戳>/），
    // 不在 .files/ 下。所以位置与存在性都以那个桶级条目为准，别去 .files/ 找。
    entry.message = "「" + record.file_name + "」是随 Bucket「" + record.bucket +
                    "」一起删除的，只能整体恢复那个桶";
    BucketService buckets(paths_, config_, logger_);
    const Result<std::vector<TrashBucket>> listed = buckets.list_trashed();
    if (ok(listed)) {
        for (const TrashBucket& candidate : std::get<std::vector<TrashBucket>>(listed)) {
            if (!iequals(candidate.original_name, record.bucket)) {
                continue;
            }
            entry.present = candidate.directory_present;
            entry.trash_path = relative_path_text(
                paths_.root(), paths_.trash() / path_from_utf8(config_.current_user) /
                                   path_from_utf8(candidate.trashed_name));
            entry.message += "（trash restore " + candidate.trashed_name + "）";
            return entry;
        }
    }
    entry.present = false;
    entry.message += "，但回收站里已经没有这个桶了";
    return entry;
}

Result<TrashEntry> TrashService::entry_of_bucket(std::string_view trashed_name) const {
    BucketService buckets(paths_, config_, logger_);
    Result<std::vector<TrashBucket>> listed = buckets.list_trashed();
    if (!ok(listed)) {
        return *error_of(listed);
    }
    for (const TrashBucket& candidate : std::get<std::vector<TrashBucket>>(listed)) {
        if (iequals(candidate.trashed_name, trashed_name)) {
            return to_entry(candidate);
        }
    }
    return make_error(ErrorCode::TrashEntryNotFound,
                      "回收站里没有这个条目：" + std::string(trashed_name));
}

Result<TrashService::Resolution> TrashService::resolve(std::string_view identifier) const {
    if (identifier.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少回收站条目名称");
    }

    BucketService buckets(paths_, config_, logger_);
    Result<std::vector<TrashBucket>> listed = buckets.list_trashed();
    if (!ok(listed)) {
        return *error_of(listed);
    }
    const std::vector<TrashBucket> bucket_entries = std::get<std::vector<TrashBucket>>(listed);

    FileService files(paths_, config_, logger_);
    Result<std::vector<FileRecord>> trashed = files.list_trashed();
    if (!ok(trashed)) {
        return *error_of(trashed);
    }
    const std::vector<FileRecord> file_entries = std::get<std::vector<FileRecord>>(trashed);

    Resolution resolution;

    // ① 桶的回收站目录名：精确、无歧义
    for (const TrashBucket& candidate : bucket_entries) {
        if (iequals(candidate.trashed_name, identifier)) {
            resolution.kind = Kind::Bucket;
            resolution.trashed_name = candidate.trashed_name;
            return resolution;
        }
    }

    // ② file_id：全局唯一。
    //    注意要查**所有**回收站记录（包括随桶删除的），否则用户按 id 找那个文件时
    //    会得到「没有这个条目」，而真相是「它在那个桶里、只能整体恢复」。
    if (const Result<FileRecord> record = files.get_by_id(identifier);
        ok(record) && std::get<FileRecord>(record).is_trash) {
        resolution.kind = Kind::File;
        resolution.file_id = std::get<FileRecord>(record).file_id;
        return resolution;
    }

    // ③ 桶的原名 ④ 文件名：都可能多条
    std::vector<TrashEntry> candidates;
    for (const TrashBucket& candidate : bucket_entries) {
        if (!candidate.original_name.empty() && iequals(candidate.original_name, identifier)) {
            candidates.push_back(to_entry(candidate));
        }
    }
    for (const FileRecord& record : file_entries) {
        if (iequals(record.file_name, identifier)) {
            const Result<TrashEntry> entry = entry_of_file(record.file_id);
            if (ok(entry)) {
                candidates.push_back(std::get<TrashEntry>(entry));
            }
        }
    }

    if (candidates.empty()) {
        return make_error(ErrorCode::TrashEntryNotFound,
                          "回收站里没有这个条目：" + std::string(identifier) +
                              "（如果是随 Bucket 删除的文件，它的整棵树挂在桶级条目下，"
                              "用 trash list 找到那个桶再整体恢复）");
    }
    if (candidates.size() > 1) {
        resolution.kind = Kind::Ambiguous;
        resolution.candidates = std::move(candidates);
        resolution.message = "回收站里有多个条目都叫「" + std::string(identifier) +
                             "」，无法确定是哪一个：请用 file_id（文件）或完整的回收站名（桶）指定";
        return resolution;
    }

    TrashEntry only = candidates.front();
    if (only.type == kTypeBucket) {
        resolution.kind = Kind::Bucket;
        resolution.trashed_name = only.id;
    } else {
        resolution.kind = Kind::File;
        resolution.file_id = only.id;
    }
    return resolution;
}

Result<std::vector<TrashEntry>> TrashService::list() {
    if (config_.current_user.empty()) {
        return make_error(ErrorCode::NoCurrentUser, "未设置当前用户");
    }

    std::vector<TrashEntry> entries;

    BucketService buckets(paths_, config_, logger_);
    Result<std::vector<TrashBucket>> listed = buckets.list_trashed();
    if (!ok(listed)) {
        return *error_of(listed);
    }
    for (const TrashBucket& candidate : std::get<std::vector<TrashBucket>>(listed)) {
        TrashEntry entry = to_entry(candidate);
        // 列表里就把「几个文件、多大」算出来：用户批准的格式里带这一项。
        // 代价是每个桶条目遍历一次目录——trash list 是显式命令，可以接受。
        if (entry.present) {
            const Result<TrashBucketDetail> detail = buckets.get_trashed(candidate.trashed_name);
            if (ok(detail)) {
                entry.files = std::get<TrashBucketDetail>(detail).file_count;
                entry.bytes = std::get<TrashBucketDetail>(detail).byte_count;
            }
        }
        entries.push_back(std::move(entry));
    }

    FileService files(paths_, config_, logger_);
    Result<std::vector<FileRecord>> trashed = files.list_trashed();
    if (!ok(trashed)) {
        return *error_of(trashed);
    }
    for (const FileRecord& record : std::get<std::vector<FileRecord>>(trashed)) {
        const Result<TrashEntry> entry = entry_of_file(record.file_id);
        if (ok(entry)) {
            entries.push_back(std::get<TrashEntry>(entry));
        }
    }

    // 最近删除的在前；时间一样时先桶后文件，保证顺序稳定
    std::sort(entries.begin(), entries.end(), [](const TrashEntry& left, const TrashEntry& right) {
        if (left.deleted_at != right.deleted_at) {
            return left.deleted_at > right.deleted_at;
        }
        if (left.type != right.type) {
            return left.type == kTypeBucket;
        }
        return left.id < right.id;
    });
    return entries;
}

Result<TrashEntry> TrashService::get(std::string_view identifier) {
    Result<Resolution> resolved = resolve(identifier);
    if (!ok(resolved)) {
        return *error_of(resolved);
    }
    const Resolution& resolution = std::get<Resolution>(resolved);
    if (resolution.kind == Kind::Ambiguous) {
        return make_error(ErrorCode::InvalidArgument, resolution.message);
    }
    if (resolution.kind == Kind::File) {
        return entry_of_file(resolution.file_id);
    }
    return entry_of_bucket(resolution.trashed_name);
}

Result<TrashCheck> TrashService::check_restore(std::string_view identifier) const {
    Result<Resolution> resolved = resolve(identifier);
    if (!ok(resolved)) {
        return *error_of(resolved);
    }
    const Resolution& resolution = std::get<Resolution>(resolved);

    TrashCheck check;
    if (resolution.kind == Kind::Ambiguous) {
        check.blocked = true;  // 确认解决不了：得让用户指定是哪一个
        check.message = resolution.message;
        for (const TrashEntry& candidate : resolution.candidates) {
            check.message += "\n  " + candidate.type + "  " + candidate.name + "（" + candidate.id + "）";
        }
        return check;
    }

    if (resolution.kind == Kind::File) {
        FileService files(paths_, config_, logger_);
        const Result<FileService::FileRestoreCheck> checked =
            files.check_restore(resolution.file_id);
        if (!ok(checked)) {
            return *error_of(checked);
        }
        const FileService::FileRestoreCheck& file_check =
            std::get<FileService::FileRestoreCheck>(checked);

        const Result<TrashEntry> entry = entry_of_file(resolution.file_id);
        if (ok(entry)) {
            check.entry = std::get<TrashEntry>(entry);
        }
        // 随桶删除 / 同名冲突 / 数据缺失：三种都不是「确认一下就能做」的事
        if (file_check.bucket_deleted || file_check.conflict || file_check.missing) {
            check.blocked = true;
            check.message = file_check.message;
        }
        return check;
    }

    const Result<TrashEntry> entry = entry_of_bucket(resolution.trashed_name);
    if (!ok(entry)) {
        return *error_of(entry);
    }
    check.entry = std::get<TrashEntry>(entry);

    if (!check.entry.restorable) {
        check.blocked = true;
        check.message = check.entry.message;
        return check;
    }
    if (!check.entry.present) {
        check.blocked = true;
        check.message = "回收站目录已不存在：" + check.entry.id;
        return check;
    }
    const std::filesystem::path target = paths_.repository() /
                                         path_from_utf8(config_.current_user) /
                                         path_from_utf8(check.entry.name);
    if (path_exists(target)) {
        // 与桶级回退的规则一致：目标已存在就不回退，不覆盖、不改名
        check.blocked = true;
        check.message = "回退失败：Bucket 已存在：" + check.entry.name;
    }
    return check;
}

Result<TrashEntry> TrashService::restore(std::string_view identifier) {
    Result<Resolution> resolved = resolve(identifier);
    if (!ok(resolved)) {
        return *error_of(resolved);
    }
    const Resolution& resolution = std::get<Resolution>(resolved);
    if (resolution.kind == Kind::Ambiguous) {
        return make_error(ErrorCode::InvalidArgument, resolution.message);
    }

    if (resolution.kind == Kind::File) {
        // 回退之后条目就不在回收站里了，所以先把信息取下来
        const Result<TrashEntry> entry = entry_of_file(resolution.file_id);
        if (!ok(entry)) {
            return *error_of(entry);
        }
        FileService files(paths_, config_, logger_);
        const Result<FileRecord> restored = files.restore(resolution.file_id);
        if (!ok(restored)) {
            return *error_of(restored);
        }
        TrashEntry done = std::get<TrashEntry>(entry);
        // **不要**把 present 改成 false：它表示「数据还在不在」，
        // 而这里的数据刚刚被搬回仓库、确实还在。回退/删除的结果由 message 说明，
        // 把它当「已经不在回收站里」用会打印出「状态：数据已不存在」这种误导信息。
        done.restorable = true;
        done.message.clear();
        return done;
    }

    const Result<TrashEntry> entry = entry_of_bucket(resolution.trashed_name);
    if (!ok(entry)) {
        return *error_of(entry);
    }
    BucketService buckets(paths_, config_, logger_);
    const Result<TrashBucket> restored = buckets.restore(resolution.trashed_name);
    if (!ok(restored)) {
        return *error_of(restored);
    }
    TrashEntry done = std::get<TrashEntry>(entry);
    done.message.clear();
    return done;
}

Result<TrashService::EmptyCheck> TrashService::check_empty() {
    const Result<std::vector<TrashEntry>> entries = list();
    if (!ok(entries)) {
        return *error_of(entries);
    }

    EmptyCheck check;
    for (const TrashEntry& entry : std::get<std::vector<TrashEntry>>(entries)) {
        if (entry.type == kTypeFile) {
            ++check.files;
        } else {
            ++check.buckets;
        }
        check.bytes += entry.bytes;
    }

    if (check.files == 0 && check.buckets == 0) {
        check.message = "回收站已经是空的";
        return check;
    }
    check.message = "永久删除回收站里的全部 " + std::to_string(check.files + check.buckets) +
                    " 项（" + std::to_string(check.files) + " 个文件、" +
                    std::to_string(check.buckets) + " 个桶，共 " + format_size(check.bytes) +
                    "），不可恢复";
    return check;
}

Result<TrashService::EmptyCheck> TrashService::empty() {
    // 每轮重新列一遍：删掉一项之后其余条目的索引/路径都会变，用旧列表接着删会大面积失败。
    EmptyCheck done;
    constexpr int kMaxRounds = 10000;  // 兜底，避免「删不掉又一直重来」
    for (int round = 0; round < kMaxRounds; ++round) {
        const Result<std::vector<TrashEntry>> entries = list();
        if (!ok(entries)) {
            return *error_of(entries);
        }
        const std::vector<TrashEntry>& remaining = std::get<std::vector<TrashEntry>>(entries);
        if (remaining.empty()) {
            break;
        }

        // 先删文件级、再删桶级：桶级条目下可能挂着文件级条目的位置关系，
        // 顺序反了容易留下「条目没了、数据还在」的孤儿。
        const TrashEntry* target = nullptr;
        for (const TrashEntry& entry : remaining) {
            if (entry.type == kTypeFile) {
                target = &entry;
                break;
            }
        }
        if (target == nullptr) {
            target = &remaining.front();
        }

        const std::string id = target->id;
        const std::string type = target->type;
        const std::uintmax_t bytes = target->bytes;
        const Result<TrashEntry> purged = purge(id);
        if (!ok(purged)) {
            // 单条失败不该卡死整个清空：记一行日志跳过它（真正的原因在那里）。
            // 下一轮还会看到它，所以最坏情况是 kMaxRounds 次后停下。
            if (logger_ != nullptr) {
                logger_->warn("Trash", "清空回收站时跳过 " + id + "：" + error_of(purged)->message);
            }
            continue;
        }
        if (type == kTypeFile) {
            ++done.files;
        } else {
            ++done.buckets;
        }
        done.bytes += bytes;
    }

    done.message = "已清空回收站：" + std::to_string(done.files) + " 个文件、" +
                   std::to_string(done.buckets) + " 个桶，共释放 " + format_size(done.bytes);
    return done;
}

Result<TrashCheck> TrashService::check_purge(std::string_view identifier) const {
    Result<Resolution> resolved = resolve(identifier);
    if (!ok(resolved)) {
        return *error_of(resolved);
    }
    const Resolution& resolution = std::get<Resolution>(resolved);

    TrashCheck check;
    if (resolution.kind == Kind::Ambiguous) {
        check.blocked = true;
        check.message = resolution.message;
        return check;
    }

    // 永久删除**永远**要确认：把要毁掉的东西说清楚
    check.needs_confirm = true;

    if (resolution.kind == Kind::File) {
        const Result<TrashEntry> entry = entry_of_file(resolution.file_id);
        if (!ok(entry)) {
            return *error_of(entry);
        }
        check.entry = std::get<TrashEntry>(entry);
        check.message = "永久删除后不可恢复：" + check.entry.name + "（Bucket " + check.entry.bucket +
                        "，" + format_size(check.entry.bytes) + "）";
        return check;
    }

    // 桶级：把「N 个文件、占多少」也带上（遍历一次目录，单条查询可以接受）
    BucketService buckets(paths_, config_, logger_);
    const Result<TrashBucketDetail> detail = buckets.get_trashed(resolution.trashed_name);
    if (!ok(detail)) {
        return *error_of(detail);
    }
    const TrashBucketDetail& found = std::get<TrashBucketDetail>(detail);
    check.entry = to_entry(found.bucket);
    check.entry.bytes = found.byte_count;
    check.entry.files = found.file_count;
    check.entry.trash_path = relative_path_text(paths_.root(), found.directory);
    check.message = "永久删除后不可恢复：" + check.entry.id + "（原桶 " +
                    (found.bucket.original_name.empty() ? std::string("未记录")
                                                        : found.bucket.original_name) +
                    "，" + std::to_string(found.file_count) + " 个文件，" +
                    format_size(found.byte_count) + "）";
    return check;
}

Result<TrashEntry> TrashService::purge(std::string_view identifier) {
    Result<Resolution> resolved = resolve(identifier);
    if (!ok(resolved)) {
        return *error_of(resolved);
    }
    const Resolution& resolution = std::get<Resolution>(resolved);
    if (resolution.kind == Kind::Ambiguous) {
        return make_error(ErrorCode::InvalidArgument, resolution.message);
    }

    if (resolution.kind == Kind::File) {
        const Result<TrashEntry> entry = entry_of_file(resolution.file_id);
        if (!ok(entry)) {
            return *error_of(entry);
        }
        FileService files(paths_, config_, logger_);
        const Result<FileRecord> purged = files.purge(resolution.file_id);
        if (!ok(purged)) {
            return *error_of(purged);
        }
        TrashEntry done = std::get<TrashEntry>(entry);
        done.message.clear();
        return done;
    }

    const Result<TrashEntry> entry = entry_of_bucket(resolution.trashed_name);
    if (!ok(entry)) {
        return *error_of(entry);
    }
    BucketService buckets(paths_, config_, logger_);
    const Result<TrashPurge> purged = buckets.purge(resolution.trashed_name);
    if (!ok(purged)) {
        return *error_of(purged);
    }
    TrashEntry done = std::get<TrashEntry>(entry);
    done.message.clear();
    return done;
}

}  // namespace fmt
