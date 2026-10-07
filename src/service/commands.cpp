#include "fmt/service/commands.hpp"

#include <string>
#include <utility>
#include <vector>

#include "fmt/bucket/bucket.hpp"
#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"

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
        if (const Status status = buckets.create(value); !ok(status)) {
            return *error_of(status);
        }

        nlohmann::json data = nlohmann::json::object();
        data["bucket"] = value;
        data["current_bucket"] = context.config.current_bucket;
        data["message"] = "Bucket 已创建：" + value +
                          (context.config.current_bucket == value ? "（已设为当前 Bucket）" : "");
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
        data["bucket"] = std::get<BucketInfo>(info).name;
        data["is_current"] = std::get<BucketInfo>(info).is_current;
        data["path"] = relative_path_text(context.paths->root(), buckets.directory_of(value));
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
        data["bucket"] = value;
        data["previous"] = previous;
        data["current_bucket"] = context.config.current_bucket;
        data["message"] = "已切换到 Bucket：" + value;
        return data;
    }

    if (operation == "bucket.delete") {
        const Result<std::string> name = argument(args, 0, "Bucket 名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);

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
            "Bucket 已删除（移入回收站）：" + value + "  ->  " + result.trashed_name;
        return data;
    }

    return make_error(ErrorCode::InvalidArgument, "未知的 Bucket 操作：" + operation);
}

// 回收站：桶级条目。文件级条目随阶段 5/7 一起进来。
Result<nlohmann::json> trash_command(AppContext& context, const std::string& operation,
                                     const nlohmann::json& args) {
    BucketService buckets(*context.paths, context.config, context.logger.get());

    if (operation == "trash.list") {
        const Result<std::vector<TrashBucket>> items = buckets.list_trashed();
        if (!ok(items)) {
            return *error_of(items);
        }

        nlohmann::json array = nlohmann::json::array();
        for (const TrashBucket& entry : std::get<std::vector<TrashBucket>>(items)) {
            nlohmann::json item = nlohmann::json::object();
            item["trashed"] = entry.trashed_name;
            item["original"] = entry.original_name;
            item["deleted_at"] = entry.deleted_at;
            item["present"] = entry.directory_present;
            array.push_back(std::move(item));
        }

        nlohmann::json data = nlohmann::json::object();
        data["deleted_buckets"] = std::move(array);
        data["count"] = data["deleted_buckets"].size();
        return data;
    }

    if (operation == "trash.restore") {
        const Result<std::string> name = argument(args, 0, "回收站条目名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);

        const Result<TrashBucket> restored = buckets.restore(value);
        if (!ok(restored)) {
            return *error_of(restored);
        }
        const TrashBucket& entry = std::get<TrashBucket>(restored);

        nlohmann::json data = nlohmann::json::object();
        data["trashed"] = entry.trashed_name;
        data["original"] = entry.original_name;
        data["restored_to"] =
            relative_path_text(context.paths->root(), buckets.directory_of(entry.original_name));
        data["message"] = "Bucket 已回退：" + entry.original_name;
        return data;
    }

    if (operation == "trash.get") {
        const Result<std::string> name = argument(args, 0, "回收站条目名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);

        const Result<TrashBucketDetail> detail = buckets.get_trashed(value);
        if (!ok(detail)) {
            return *error_of(detail);
        }
        const TrashBucketDetail& found = std::get<TrashBucketDetail>(detail);

        nlohmann::json data = nlohmann::json::object();
        data["trashed"] = found.bucket.trashed_name;
        data["original"] = found.bucket.original_name;
        data["deleted_at"] = found.bucket.deleted_at;
        data["present"] = found.bucket.directory_present;
        data["path"] = relative_path_text(context.paths->root(), found.directory);
        data["files"] = found.file_count;
        data["bytes"] = found.byte_count;
        return data;
    }

    if (operation == "trash.delete") {
        // **永久删除不可恢复**：调用方必须显式确认（CLI 在用户回答 y 之后才置 force）。
        // 少一次误操作就少一次数据丢失，宁可多要一个字段。
        const auto force = args.find("force");
        if (force == args.end() || !force->is_boolean() || !force->get<bool>()) {
            return make_error(ErrorCode::InvalidArgument,
                              "永久删除不可恢复，需要确认（force = true）");
        }

        const Result<std::string> name = argument(args, 0, "回收站条目名称");
        if (!ok(name)) {
            return *error_of(name);
        }
        const std::string value = std::get<std::string>(name);

        const Result<TrashPurge> purged = buckets.purge(value);
        if (!ok(purged)) {
            return *error_of(purged);
        }
        const TrashPurge& result = std::get<TrashPurge>(purged);

        nlohmann::json data = nlohmann::json::object();
        data["trashed"] = result.trashed_name;
        data["original"] = result.original_name;
        data["removed_files"] = result.removed_files;
        data["removed_records"] = result.removed_records;
        data["message"] = "已永久删除：" + result.trashed_name + "（" +
                          std::to_string(result.removed_files) + " 个文件，" +
                          std::to_string(result.removed_records) + " 条记录）";
        return data;
    }

    return make_error(ErrorCode::InvalidArgument, "未知的回收站操作：" + operation);
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

    // 已经登记、还没实现的模块（file / share / config / server）。
    return make_error(ErrorCode::ServiceOperationFailed, "操作尚未实现：" + operation);
}

}  // namespace fmt::service
