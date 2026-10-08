#include "fmt/service/commands.hpp"

#include <string>
#include <utility>
#include <vector>

#include "fmt/bucket/bucket.hpp"
#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/file/file.hpp"
#include "fmt/trash/trash.hpp"

namespace fmt::service {
namespace {

// 取第 index 个位置参数；缺失或类型不对都算参数错误（FMT-001）。
Result<std::string> argument(const nlohmann::json& args, std::size_t index, const char* what) {
    const auto iterator = args.find("argv");
    if (iterator == args.end() || !iterator->is_array() || iterator->size() <= index) {
        return make_error(ErrorCode::InvalidArgument, std::string("缺少") + what);
    }
    const nlohmann::json& value = (*iterator)[index];
    if (!value.is_string()) {
        return make_error(ErrorCode::InvalidArgument, std::string(what) + "必须是字符串");
    }
    return value.get<std::string>();
}

Result<nlohmann::json> bucket_command(AppContext& context, const std::string& operation,
                                      const nlohmann::json& args) {
    BucketService buckets(*context.paths, context.config, context.logger.get());

    if (operation == "bucket.create") {
        const Result<std::string> name = argument(args, 0, "Bucket 名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);
        const Result<BucketCreation> created = buckets.create(value);
        if (!ok(created)) {
            return *error_of(created);
        }
        const BucketCreation& result = std::get<BucketCreation>(created);

        nlohmann::json data = nlohmann::json::object();
        data["bucket"] = result.name;
        data["current_bucket"] = context.config.current_bucket;
        data["message"] = "Bucket 已创建：" + result.name +
                          (result.became_current ? "（已设为当前 Bucket）" : "");
        if (result.renamed) {
            data["note"] = "Bucket 名称统一使用小写：已把 " + result.requested + " 转为 " +
                           result.name;
        }
        return data;
    }

    if (operation == "bucket.list") {
        const Result<std::vector<BucketInfo>> items = buckets.list();
        if (!ok(items)) {
            return *error_of(items);
        }

        nlohmann::json array = nlohmann::json::array();
        for (const BucketInfo& info : std::get<std::vector<BucketInfo>>(items)) {
            nlohmann::json entry = nlohmann::json::object();
            entry["name"] = info.name;
            entry["is_current"] = info.is_current;
            array.push_back(std::move(entry));
        }

        nlohmann::json data = nlohmann::json::object();
        data["buckets"] = std::move(array);
        data["count"] = data["buckets"].size();
        data["current_bucket"] = context.config.current_bucket;
        return data;
    }

    if (operation == "bucket.get") {
        const Result<std::string> name = argument(args, 0, "Bucket 名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);

        const Result<BucketInfo> info = buckets.get(value);
        if (!ok(info)) {
            return *error_of(info);
        }

        nlohmann::json data = nlohmann::json::object();
        const BucketInfo& found = std::get<BucketInfo>(info);
        data["bucket"] = found.name;
        data["is_current"] = found.is_current;
        // 路径也用**规范化后的名字**：否则 `bucket get WORK` 会打印 .../WORK，
        // 而同一个响应里的 bucket 字段是 work，自相矛盾。
        data["path"] = relative_path_text(context.paths->root(), buckets.directory_of(found.name));
        return data;
    }

    if (operation == "bucket.use") {
        const Result<std::string> name = argument(args, 0, "Bucket 名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);
        const std::string previous = context.config.current_bucket;

        if (const Status status = buckets.use(value); !ok(status)) {
            return *error_of(status);
        }

        nlohmann::json data = nlohmann::json::object();
        data["bucket"] = context.config.current_bucket;  // 已规范化的实际桶名
        data["previous"] = previous;
        data["current_bucket"] = context.config.current_bucket;
        data["message"] = "已切换到 Bucket：" + context.config.current_bucket;
        if (value != context.config.current_bucket) {
            data["note"] = "Bucket 名称统一使用小写：已把 " + value + " 规范为 " +
                           context.config.current_bucket;
        }
        return data;
    }

    if (operation == "bucket.delete") {
        const Result<std::string> name = argument(args, 0, "Bucket 名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);

        // ① 预检（dry_run）：桶里有东西就提醒「之后只能整体恢复这个桶」
        if (args.value("dry_run", false)) {
            const Result<BucketDeleteCheck> checked = buckets.check_remove(value);
            if (!ok(checked)) {
                return *error_of(checked);
            }
            const BucketDeleteCheck& check = std::get<BucketDeleteCheck>(checked);

            nlohmann::json data = nlohmann::json::object();
            data["bucket"] = check.bucket;
            data["is_current"] = check.is_current;
            data["files"] = check.files;
            data["bytes"] = check.bytes;
            data["has_content"] = check.has_content;
            // 空桶不打扰用户；有内容才要一次确认
            data["needs_confirm"] = check.has_content;
            data["blocked"] = false;
            if (!check.message.empty()) {
                data["message"] = check.message;
            }
            return data;
        }

        // ② 兜底：预检被绕过时，有内容的桶也必须确认过才删。
        const auto force = args.find("force");
        const bool forced = force != args.end() && force->is_boolean() && force->get<bool>();
        if (!forced) {
            const Result<BucketDeleteCheck> checked = buckets.check_remove(value);
            if (!ok(checked)) {
                return *error_of(checked);
            }
            const BucketDeleteCheck& check = std::get<BucketDeleteCheck>(checked);
            if (check.has_content) {
                return make_error(ErrorCode::ConfirmRequired,
                                  check.message + "；确认删除请加 force（CLI：--yes）");
            }
        }

        const Result<BucketRemoval> removal = buckets.remove(value);
        if (!ok(removal)) {
            return *error_of(removal);
        }
        const BucketRemoval& result = std::get<BucketRemoval>(removal);

        nlohmann::json data = nlohmann::json::object();
        data["bucket"] = value;
        data["moved_to"] = relative_path_text(context.paths->root(), result.moved_to);
        data["trashed_name"] = result.trashed_name;
        data["files_affected"] = result.files_affected;
        data["was_current"] = result.was_current;
        data["current_bucket"] = context.config.current_bucket;
        data["message"] =
            "Bucket 已删除（移入回收站）：" + value + "  ->  " + result.trashed_name +
            "（之后只能整体恢复这个桶）";
        return data;
    }

    return make_error(ErrorCode::InvalidArgument, "未知的 Bucket 操作：" + operation);
}

// 回收站：桶级条目。文件级条目随阶段 5/7 一起进来。
Result<nlohmann::json> trash_command(AppContext& context, const std::string& operation,
                                     const nlohmann::json& args) {
    TrashService trash(*context.paths, context.config, context.logger.get());

    const auto entry_json = [](const TrashEntry& entry) {
        nlohmann::json item = nlohmann::json::object();
        item["type"] = entry.type;  // file / bucket：CLI 据此标注
        item["id"] = entry.id;      // 文件=file_id；桶=回收站目录名
        item["name"] = entry.name;
        item["bucket"] = entry.bucket;
        item["deleted_at"] = entry.deleted_at;
        item["bytes"] = entry.bytes;
        item["files"] = entry.files;
        item["present"] = entry.present;
        item["restorable"] = entry.restorable;
        if (!entry.trash_path.empty()) {
            item["trash_path"] = entry.trash_path;
        }
        if (!entry.message.empty()) {
            item["reason"] = entry.message;
        }
        return item;
    };

    // 预检结论：说清楚 + 要不要确认 + 能不能靠确认解决
    const auto check_json = [&entry_json](const TrashCheck& check) {
        nlohmann::json data = nlohmann::json::object();
        data["needs_confirm"] = check.needs_confirm;
        data["blocked"] = check.blocked;
        data["entry"] = entry_json(check.entry);
        if (!check.message.empty()) {
            data["message"] = check.message;
        }
        return data;
    };

    if (operation == "trash.list") {
        const Result<std::vector<TrashEntry>> items = trash.list();
        if (!ok(items)) {
            return *error_of(items);
        }

        nlohmann::json array = nlohmann::json::array();
        std::size_t files = 0;
        std::size_t buckets = 0;
        for (const TrashEntry& entry : std::get<std::vector<TrashEntry>>(items)) {
            if (entry.type == "file") {
                ++files;
            } else {
                ++buckets;
            }
            array.push_back(entry_json(entry));
        }

        nlohmann::json data = nlohmann::json::object();
        data["entries"] = std::move(array);
        data["count"] = data["entries"].size();
        data["files"] = files;
        data["buckets"] = buckets;
        return data;
    }

    if (operation == "trash.get") {
        const Result<std::string> name = argument(args, 0, "回收站条目名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const Result<TrashEntry> entry = trash.get(std::get<std::string>(name));
        if (!ok(entry)) {
            return *error_of(entry);
        }

        nlohmann::json data = nlohmann::json::object();
        data["entry"] = entry_json(std::get<TrashEntry>(entry));
        return data;
    }

    if (operation == "trash.restore") {
        const Result<std::string> name = argument(args, 0, "回收站条目名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);

        // 预检：同名冲突 / 随桶删除 / 数据缺失 —— 三种都不是「确认一下就能做」的事
        if (args.value("dry_run", false)) {
            const Result<TrashCheck> checked = trash.check_restore(value);
            if (!ok(checked)) {
                return *error_of(checked);
            }
            return check_json(std::get<TrashCheck>(checked));
        }

        const Result<TrashEntry> restored = trash.restore(value);
        if (!ok(restored)) {
            return *error_of(restored);
        }
        const TrashEntry& entry = std::get<TrashEntry>(restored);

        nlohmann::json data = nlohmann::json::object();
        data["entry"] = entry_json(entry);
        data["message"] =
            (entry.type == "file" ? "文件已回退：" : "Bucket 已回退：") + entry.name;
        return data;
    }

    if (operation == "trash.delete") {
        const Result<std::string> name = argument(args, 0, "回收站条目名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);

        // 预检：把要永久删掉的东西说清楚
        if (args.value("dry_run", false)) {
            const Result<TrashCheck> checked = trash.check_purge(value);
            if (!ok(checked)) {
                return *error_of(checked);
            }
            return check_json(std::get<TrashCheck>(checked));
        }

        // 永久删除不可恢复：调用方必须显式确认（CLI 在用户回答 y 之后才置 force）
        const auto force = args.find("force");
        if (force == args.end() || !force->is_boolean() || !force->get<bool>()) {
            return make_error(ErrorCode::ConfirmRequired,
                              "永久删除不可恢复，需要确认（force = true）");
        }

        const Result<TrashEntry> purged = trash.purge(value);
        if (!ok(purged)) {
            return *error_of(purged);
        }
        const TrashEntry& entry = std::get<TrashEntry>(purged);

        nlohmann::json data = nlohmann::json::object();
        data["entry"] = entry_json(entry);
        data["message"] = "已永久删除：" + entry.name;
        return data;
    }

    return make_error(ErrorCode::InvalidArgument, "未知的回收站操作：" + operation);
}

// 文件：list / get / delete。**upload 不在这里**——它要边下载边写盘，属长任务，
// 由运行体的两段式路径处理（下载在锁外、登记在锁内，见 file.hpp）。
Result<nlohmann::json> file_command(AppContext& context, const std::string& operation,
                                    const nlohmann::json& args) {
    FileService files(*context.paths, context.config, context.logger.get());

    if (operation == "file.list") {
        const Result<std::vector<FileRecord>> items = files.list();
        if (!ok(items)) {
            return *error_of(items);
        }

        nlohmann::json array = nlohmann::json::array();
        for (const FileRecord& record : std::get<std::vector<FileRecord>>(items)) {
            nlohmann::json item = nlohmann::json::object();
            item["file_id"] = record.file_id;
            item["file_name"] = record.file_name;
            item["extension"] = record.extension;
            item["file_type"] = record.file_type;
            item["size"] = record.size;
            item["md5"] = record.md5;
            array.push_back(std::move(item));
        }

        nlohmann::json data = nlohmann::json::object();
        data["files"] = std::move(array);
        data["count"] = data["files"].size();
        data["current_bucket"] = context.config.current_bucket;
        return data;
    }

    if (operation == "file.get") {
        const Result<std::string> name = argument(args, 0, "file_id 或文件名");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);

        // 先当 file_id 查（全局唯一），查不到再当文件名查（当前用户 + 正常文件）。
        Result<FileRecord> record = files.get_by_id(value);
        if (!ok(record)) {
            record = files.get_by_name(value);
        }
        if (!ok(record)) {
            return *error_of(record);
        }

        const FileRecord& found = std::get<FileRecord>(record);
        nlohmann::json data = nlohmann::json::object();
        data["file_id"] = found.file_id;
        data["file_name"] = found.file_name;
        data["bucket"] = found.bucket;
        data["extension"] = found.extension;
        data["file_type"] = found.file_type;
        data["size"] = found.size;
        data["md5"] = found.md5;
        data["is_trash"] = found.is_trash;
        data["trash_reason"] = found.trash_reason;

        const Result<std::filesystem::path> path = files.resolve_path(found);
        if (ok(path)) {
            data["path"] =
                relative_path_text(context.paths->root(), std::get<std::filesystem::path>(path));
        }
        // 在回收站里的记录，仓库里当然找不到——此时告诉用户**它在回收站哪儿**。
        if (found.is_trash) {
            const Result<std::filesystem::path> trashed = files.trash_path_of(found);
            if (ok(trashed)) {
                data["trash_path"] = relative_path_text(
                    context.paths->root(), std::get<std::filesystem::path>(trashed));
            }
        }
        return data;
    }

    if (operation == "file.delete") {
        const Result<std::string> name = argument(args, 0, "file_id 或文件名");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);

        // ① 预检（dry_run）：只读，不改任何东西。
        //    CLI 用它实现「先检查 → 说清楚冲突的具体对象 → 再让用户确认」，
        //    这样**破坏性请求在用户确认之前一个都不发**。
        if (args.value("dry_run", false)) {
            const Result<FileDeleteCheck> checked = files.check_remove(value);
            if (!ok(checked)) {
                return *error_of(checked);
            }
            const FileDeleteCheck& check = std::get<FileDeleteCheck>(checked);

            nlohmann::json data = nlohmann::json::object();
            data["ambiguous"] = check.ambiguous;
            data["other_bucket"] = check.other_bucket;
            // 歧义**不能**靠 y/N 解决（y 无法表达删哪一个），所以它算「阻断」而不是「确认」
            data["blocked"] = check.ambiguous;
            data["needs_confirm"] = check.other_bucket;
            data["current_bucket"] = check.current_bucket;
            if (!check.file_id.empty()) {
                data["file_id"] = check.file_id;
                data["file_name"] = check.file_name;
                data["bucket"] = check.bucket;
            }
            if (!check.path.empty()) {
                data["path"] = check.path;
            }
            if (!check.candidates.empty()) {
                nlohmann::json array = nlohmann::json::array();
                for (const FileRecord& candidate : check.candidates) {
                    nlohmann::json item = nlohmann::json::object();
                    item["file_id"] = candidate.file_id;
                    item["file_name"] = candidate.file_name;
                    item["bucket"] = candidate.bucket;
                    array.push_back(std::move(item));
                }
                data["candidates"] = std::move(array);
            }
            if (!check.message.empty()) {
                data["message"] = check.message;
            }
            return data;
        }

        // ② 兜底：预检被绕过时，跨 Bucket 的删除仍然必须显式确认过。
        const auto force = args.find("force");
        const bool forced = force != args.end() && force->is_boolean() && force->get<bool>();
        if (!forced) {
            const Result<FileDeleteCheck> checked = files.check_remove(value);
            if (!ok(checked)) {
                return *error_of(checked);
            }
            const FileDeleteCheck& check = std::get<FileDeleteCheck>(checked);
            if (check.other_bucket) {
                return make_error(ErrorCode::ConfirmRequired,
                                  check.message + "；确认删除请加 force（CLI：--yes）");
            }
        }

        const Result<FileRecord> removed = files.remove(value);
        if (!ok(removed)) {
            return *error_of(removed);
        }
        const FileRecord& record = std::get<FileRecord>(removed);

        nlohmann::json data = nlohmann::json::object();
        data["file_id"] = record.file_id;
        data["file_name"] = record.file_name;
        data["bucket"] = record.bucket;

        const Result<std::filesystem::path> target = files.trash_path_of(record);
        if (ok(target)) {
            data["moved_to"] =
                relative_path_text(context.paths->root(), std::get<std::filesystem::path>(target));
        }
        data["message"] = "文件已移入回收站：" + record.file_name +
                          (iequals(record.bucket, context.config.current_bucket)
                               ? std::string{}
                               : "（Bucket：" + record.bucket + "）");
        return data;
    }

    return make_error(ErrorCode::ServiceOperationFailed, "操作尚未实现：" + operation);
}

}  // namespace

bool is_known_business(const std::string& operation) {
    for (const std::string_view prefix :
         {"bucket.", "file.", "share.", "trash.", "config.", "server."}) {
        if (starts_with(operation, prefix)) {
            return true;
        }
    }
    return false;
}

Result<nlohmann::json> execute_business(AppContext& context, const std::string& operation,
                                        const nlohmann::json& args) {
    if (context.paths == nullptr) {
        return make_error(ErrorCode::ServiceOperationFailed, "服务尚未初始化数据根");
    }

    if (starts_with(operation, "bucket.")) {
        return bucket_command(context, operation, args);
    }

    if (starts_with(operation, "trash.")) {
        return trash_command(context, operation, args);
    }

    if (starts_with(operation, "file.")) {
        return file_command(context, operation, args);
    }

    // 已经登记、还没实现的模块（share / config / server）。
    return make_error(ErrorCode::ServiceOperationFailed, "操作尚未实现：" + operation);
}

}  // namespace fmt::service
