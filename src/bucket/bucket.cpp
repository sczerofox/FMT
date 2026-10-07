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

// "2026-10-08 01:23:45" -> "20261008012345"，给回收站里重名的目录做后缀。
std::string compact_stamp() {
    std::string stamp = local_timestamp();
    stamp.erase(std::remove_if(stamp.begin(), stamp.end(),
                               [](char ch) { return ch == '-' || ch == ':' || ch == ' '; }),
                stamp.end());
    return stamp;
}

}  // namespace

BucketService::BucketService(const PathManager& paths, Config& config, Logger* logger)
    : paths_(paths), config_(config), logger_(logger) {}

std::filesystem::path BucketService::user_root() const {
    return paths_.repository() / path_from_utf8(config_.current_user);
}

std::filesystem::path BucketService::directory_of(std::string_view name) const {
    return bucket_path(name);
}

std::filesystem::path BucketService::bucket_path(std::string_view name) const {
    return user_root() / path_from_utf8(std::string(name));
}

std::filesystem::path BucketService::trash_bucket_path(std::string_view name) const {
    return paths_.trash() / path_from_utf8(config_.current_user) /
           path_from_utf8(std::string(name));
}

Status BucketService::require_current_user() const {
    if (config_.current_user.empty()) {
        return make_error(ErrorCode::NoCurrentUser, "未设置当前用户");
    }
    return std::monostate{};
}

Status BucketService::persist_config() { return save_config(paths_, config_); }

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
            info.is_current = (info.name == config_.current_bucket);
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

    BucketRemoval removal;
    removal.was_current = (config_.current_bucket == name);

    // 顺序按第 30 节：移动数据 -> 相关文件 is_trash -> 写 Trash metadata -> 处理当前 Bucket。
    if (const Status status = move_bucket_to_trash(name, &removal.moved_to); !ok(status)) {
        return *error_of(status);
    }

    if (const Status status = mark_bucket_files_trashed(name, &removal.files_affected);
        !ok(status)) {
        return *error_of(status);
    }

    if (const Status status = append_trash_record(name, bucket_path(name), removal.moved_to);
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
        logger_->info("Bucket", "删除 Bucket：" + std::string(name) + "（移入回收站，" +
                                    std::to_string(removal.files_affected) +
                                    " 个文件标记为已删除）");
    }
    return removal;
}

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

Status BucketService::move_bucket_to_trash(std::string_view name, std::filesystem::path* moved_to) {
    const std::filesystem::path source = bucket_path(name);
    const std::filesystem::path target = trash_bucket_path(name);

    if (const Status status = ensure_directory(target.parent_path()); !ok(status)) {
        return status;
    }

    // 回收站里已经有同名目录（删过、又建了同名 Bucket）：给新来的加时间后缀，
    // 不覆盖已经躺在回收站里的数据，trash_path 记实际位置。
    std::filesystem::path destination = target;
    if (path_exists(destination)) {
        destination = target.parent_path() /
                      (target.filename().wstring() + L"_" + to_wide(compact_stamp()));
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

Status BucketService::mark_bucket_files_trashed(std::string_view name, std::size_t* affected) {
    if (affected != nullptr) {
        *affected = 0;
    }
    if (!file_exists(paths_.file_data())) {
        return std::monostate{};  // 还没有 file.json：没有文件要标记
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
        if (record.value("is_trash", false)) {
            continue;  // 已经在回收站里
        }
        record["is_trash"] = true;
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

Status BucketService::append_trash_record(std::string_view name, const std::filesystem::path& from,
                                          const std::filesystem::path& to) {
    nlohmann::json document = make_collection(1, "trash");
    if (file_exists(paths_.trash_data())) {
        Result<nlohmann::json> parsed = read_json_file(paths_.trash_data());
        if (!ok(parsed)) {
            return *error_of(parsed);
        }
        document = std::get<nlohmann::json>(parsed);
        if (const Status version = check_version(document, 1); !ok(version)) {
            return version;
        }
    }

    // Bucket 级记录：**没有 file_id**，用 user + bucket 标识（第 17 节）。
    nlohmann::json record = nlohmann::json::object();
    record["type"] = "bucket";
    record["user"] = config_.current_user;
    record["bucket"] = std::string(name);
    record["original_path"] = relative_path_text(paths_.root(), from);
    record["trash_path"] = relative_path_text(paths_.root(), to);
    record["deleted_at"] = local_datetime_iso();

    document["trash"].push_back(std::move(record));
    return write_json_file(paths_.trash_data(), document);
}

}  // namespace fmt
