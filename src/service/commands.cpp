#include "fmt/service/commands.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "fmt/bucket/bucket.hpp"
#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/file/file.hpp"
#include "fmt/share/share.hpp"
#include "fmt/trash/trash.hpp"
#include "fmt/user/user.hpp"

namespace fmt::service {
namespace {


// 分页上限：一次最多 1000 条。不设上限的话，page_size 写个天文数字就等于
// 让服务端一次吐几十万个文件。
constexpr int kMaxPageSize = 1000;

// 从 args 里取一个整数开关。
// **不能直接用 args.value<int>()**：类型不匹配时 nlohmann 会抛 type_error，
// 而在服务端抛异常等于 500（历史上还因此 abort 过一次）。
// 另外 args 本身可能是 null（请求没带信封）：find/value 在非对象上同样会抛，
// 所以第一件事是确认它是个对象。
Result<int> int_arg(const nlohmann::json& args, const char* key, int fallback) {
    if (!args.is_object()) {
        return fallback;
    }
    const auto found = args.find(key);
    if (found == args.end() || found->is_null()) {
        return fallback;
    }
    if (!found->is_number_integer()) {
        return make_error(ErrorCode::InvalidArgument, std::string(key) + " 必须是整数");
    }
    return found->get<int>();
}

// 取一个字符串开关：不是对象 / 没有这个键 / 类型不对，一律按缺省，绝不抛异常。
std::string text_arg(const nlohmann::json& args, const char* key, const std::string& fallback) {
    if (!args.is_object()) {
        return fallback;
    }
    const auto found = args.find(key);
    if (found == args.end() || !found->is_string()) {
        return fallback;
    }
    return found->get<std::string>();
}

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
    // 位置参数先清掉粘贴污染：Explorer 的「复制路径」套的引号、聊天窗口/网页带进来的
    // U+202A 这类不可见字符。它们会让「路径/名字明明是对的」却查不到——在这里一处收口。
    return clean_user_path(value.get<std::string>());
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

    if (operation == "trash.empty") {
        // 永久删除整站：先把「几项、多少」说清楚，再要一次确认（缺 force → FMT-016）。
        const auto force = args.find("force");
        const bool forced = force != args.end() && force->is_boolean() && force->get<bool>();

        if (args.value("dry_run", false) || !forced) {
            const Result<TrashService::EmptyCheck> checked = trash.check_empty();
            if (!ok(checked)) {
                return *error_of(checked);
            }
            const TrashService::EmptyCheck& check = std::get<TrashService::EmptyCheck>(checked);
            const bool has_content = (check.files + check.buckets) > 0;

            // 空的就别让人为一个空操作再确认一次：直接当成功返回。
            if (!has_content && !args.value("dry_run", false)) {
                nlohmann::json empty = nlohmann::json::object();
                empty["files"] = 0;
                empty["buckets"] = 0;
                empty["bytes"] = 0;
                empty["message"] = check.message;
                return empty;
            }
            if (has_content && !forced && !args.value("dry_run", false)) {
                return make_error(ErrorCode::ConfirmRequired,
                                  check.message + "；确认清空请加 force（CLI：--yes）");
            }

            nlohmann::json data = nlohmann::json::object();
            data["files"] = check.files;
            data["buckets"] = check.buckets;
            data["bytes"] = check.bytes;
            data["needs_confirm"] = has_content;
            data["blocked"] = false;
            data["message"] = check.message;
            return data;
        }

        const Result<TrashService::EmptyCheck> emptied = trash.empty();
        if (!ok(emptied)) {
            return *error_of(emptied);
        }
        const TrashService::EmptyCheck& done = std::get<TrashService::EmptyCheck>(emptied);
        nlohmann::json data = nlohmann::json::object();
        data["files"] = done.files;
        data["buckets"] = done.buckets;
        data["bytes"] = done.bytes;
        data["message"] = done.message;
        return data;
    }

    return make_error(ErrorCode::InvalidArgument, "未知的回收站操作：" + operation);
}

// 文件：list / get / delete。**upload 不在这里**——它要边下载边写盘，属长任务，
// 由运行体的两段式路径处理（下载在锁外、登记在锁内，见 file.hpp）。
Result<nlohmann::json> file_command(AppContext& context, const std::string& operation,
                                    const nlohmann::json& args) {
    FileService files(*context.paths, context.config, context.logger.get());

    if (operation == "file.upload_stream") {
        // **流式上传的第二段**：数据已经由 HTTP 层边收边写落在 temp/ 里，
        // 这里补算大小与 MD5 并入库（一次移动，不二次拷贝）。
        // 文件名由 HTTP 层从 ?name= 或 Content-Disposition 里取出来传进来。
        const Result<std::string> staged = argument(args, 0, "暂存文件路径");
        if (!ok(staged)) {
            return *error_of(staged);
        }
        std::string name;
        if (const Result<std::string> given = argument(args, 1, "文件名"); ok(given)) {
            name = std::get<std::string>(given);
        }

        Result<PreparedUpload> prepared =
            prepare_staged_upload(*context.paths, path_from_utf8(std::get<std::string>(staged)), name,
                                  context.config.max_upload_size, context.logger.get());
        if (!ok(prepared)) {
            // 暂存文件的生命周期到这里为止：失败就地清掉，别留在 temp/ 里。
            std::error_code ignored;
            std::filesystem::remove(path_from_utf8(std::get<std::string>(staged)), ignored);
            return *error_of(prepared);
        }

        FileService files(*context.paths, context.config, context.logger.get());
        const Result<FileRecord> record = files.commit_upload(std::get<PreparedUpload>(prepared));
        if (!ok(record)) {
            return *error_of(record);  // commit_upload 自己负责删暂存文件
        }
        const FileRecord& saved = std::get<FileRecord>(record);
        nlohmann::json data = nlohmann::json::object();
        data["file_id"] = saved.file_id;
        data["file_name"] = saved.file_name;
        data["size"] = saved.size;
        data["md5"] = saved.md5;
        data["extension"] = saved.extension;
        data["file_type"] = saved.file_type;
        data["bucket"] = saved.bucket;
        data["message"] = "流式上传完成：" + saved.file_name + "（" + saved.file_id + "）";
        return data;
    }
    if (operation == "file.list") {
        const Result<std::vector<FileRecord>> items = files.list();
        if (!ok(items)) {
            return *error_of(items);
        }
        std::vector<FileRecord> records = std::get<std::vector<FileRecord>>(items);

        // ---- 搜索（先过滤）----
        // 关键字按**不区分大小写的子串**匹配文件名，也匹配 file_id
        //（file_id 是用户手里常有的东西，能搜到省一次 get）。
        const std::string search = trim(text_arg(args, "search", std::string{}));
        if (!search.empty()) {
            const std::string needle = to_lower(search);
            records.erase(std::remove_if(records.begin(), records.end(),
                                         [&needle](const FileRecord& record) {
                                             return to_lower(record.file_name).find(needle) ==
                                                        std::string::npos &&
                                                    to_lower(record.file_id).find(needle) ==
                                                        std::string::npos;
                                         }),
                          records.end());
        }
        const std::size_t total = records.size();

        // 排序：默认按名字（不区分大小写，同级用 file_id 保证稳定）；
        // size 大的在前；id 就是入库顺序（file_id 里的日期+序号天然递增）。
        const std::string sort = text_arg(args, "sort", std::string("name"));
        if (sort == "name") {
            std::sort(records.begin(), records.end(),
                      [](const FileRecord& left, const FileRecord& right) {
                          const std::string a = to_lower(left.file_name);
                          const std::string b = to_lower(right.file_name);
                          return a == b ? left.file_id < right.file_id : a < b;
                      });
        } else if (sort == "size") {
            std::sort(records.begin(), records.end(),
                      [](const FileRecord& left, const FileRecord& right) {
                          return left.size == right.size ? left.file_id < right.file_id
                                                         : left.size > right.size;
                      });
        } else if (sort == "id") {
            std::sort(records.begin(), records.end(),
                      [](const FileRecord& left, const FileRecord& right) {
                          return left.file_id < right.file_id;
                      });
        } else {
            return make_error(ErrorCode::InvalidArgument,
                              "排序方式只能是 name / size / id，收到：" + sort);
        }

        // ---- 分页（**必须排在排序之后**）----
        // 顺序是：过滤 -> 排序 -> 切片。排序与切片之间不能有随机性，否则翻页
        // 会漏文件或重复（同一个文件在两页里各出现一次，是最典型的分页 bug）。
        //   page_size 缺省或 0 = 不分页（保持老行为，一次给全，只是多回 total）
        //   page 从 1 开始；给了 page_size 才分页
        const Result<int> page = int_arg(args, "page", 1);
        if (!ok(page)) {
            return *error_of(page);
        }
        const Result<int> page_size = int_arg(args, "page_size", 0);
        if (!ok(page_size)) {
            return *error_of(page_size);
        }
        const int wanted_page = std::get<int>(page);
        const int wanted_size = std::get<int>(page_size);
        if (wanted_page < 1) {
            return make_error(ErrorCode::InvalidArgument, "page 从 1 开始，收到：" +
                                                              std::to_string(wanted_page));
        }
        if (wanted_size < 0 || wanted_size > kMaxPageSize) {
            return make_error(ErrorCode::InvalidArgument,
                              "page_size 只能是 0（不分页）到 " + std::to_string(kMaxPageSize) +
                                  "，收到：" + std::to_string(wanted_size));
        }

        std::vector<FileRecord> page_records;
        std::size_t total_pages = 0;
        if (wanted_size > 0) {
            total_pages = (total + static_cast<std::size_t>(wanted_size) - 1) /
                          static_cast<std::size_t>(wanted_size);
            const std::size_t begin =
                static_cast<std::size_t>(wanted_page - 1) * static_cast<std::size_t>(wanted_size);
            if (begin < total) {  // 超出末页就是空页，不是错误
                const std::size_t end = std::min(total, begin + static_cast<std::size_t>(wanted_size));
                page_records.assign(records.begin() + static_cast<std::ptrdiff_t>(begin),
                                    records.begin() + static_cast<std::ptrdiff_t>(end));
            }
        } else {
            page_records = records;
        }

        nlohmann::json array = nlohmann::json::array();
        // 注意：遍历的是**过滤+排序+分页之后**的 page_records，不是 items。
        for (const FileRecord& record : page_records) {
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
        data["count"] = data["files"].size();   // 本页条数
        data["total"] = total;                  // 命中总数（过滤之后、分页之前）
        data["page"] = wanted_page;
        data["page_size"] = wanted_size;        // 0 = 不分页
        data["total_pages"] = total_pages;
        if (!search.empty()) {
            data["search"] = search;
        }
        // **只输出文件的信息**（用户要求）：不再附带 current_bucket 之类的环境字段。
        data["sort"] = sort;
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

        // 相对数据根的路径：HTTP 的下载/预览要按它去读文件。
        // **只给单条查询**，列表不加——用户明确要求列表只输出文件信息。
        if (const Result<std::filesystem::path> absolute = files.resolve_path(found); ok(absolute)) {
            data["path"] = relative_path_text(context.paths->root(),
                                             std::get<std::filesystem::path>(absolute));
        }
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

// 解析「大小」写法：纯字节数（"10485760"），或带单位（"10MB" / "10 MB" / "512KB" / "1GB"）。
// 只认 KB/MB/GB 三种十进制单位——配置里给的本来就是十进制换算（1 MB = 1024 * 1024 字节）。
Result<std::uintmax_t> parse_size_value(const std::string& text) {
    std::string value = to_lower(trim(text));
    if (value.empty()) {
        return make_error(ErrorCode::InvalidArgument, "大小不能为空");
    }

    std::uintmax_t multiplier = 1;
    for (const auto& unit : {std::pair<std::string, std::uintmax_t>{"kb", 1024},
                             {"mb", 1024 * 1024},
                             {"gb", 1024 * 1024 * 1024}}) {
        if (ends_with(value, unit.first)) {
            multiplier = unit.second;
            value = trim(value.substr(0, value.size() - unit.first.size()));
            break;
        }
    }
    if (value.empty()) {
        return make_error(ErrorCode::InvalidArgument, "大小缺少数字：" + text);
    }

    std::uintmax_t number = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return make_error(ErrorCode::InvalidArgument,
                              "大小只能是数字（可带 KB/MB/GB）：" + text);
        }
        number = number * 10 + static_cast<std::uintmax_t>(ch - '0');
    }

    constexpr std::uintmax_t kMinBytes = 1024;                 // 1 KB：再小没有意义
    constexpr std::uintmax_t kMaxBytes = 100ull * 1024 * 1024 * 1024;  // 100 GB：兜住溢出
    const std::uintmax_t bytes = number * multiplier;
    if (bytes < kMinBytes) {
        return make_error(ErrorCode::InvalidArgument, "上限太小（至少 1KB）：" + text);
    }
    if (number > 0 && bytes / multiplier != number) {
        return make_error(ErrorCode::InvalidArgument, "大小超出可表示范围：" + text);
    }
    if (bytes > kMaxBytes) {
        return make_error(ErrorCode::InvalidArgument, "上限过大（最多 100GB）：" + text);
    }
    return bytes;
}

// config.*：V1 只让改 max_upload_size，其余只读。
// 之所以只开这一项：它是唯一需要按机器/网络情况调整的值（其他项改了也没有对应行为）。
Result<nlohmann::json> config_command(AppContext& context, const std::string& operation,
                                      const nlohmann::json& args) {
    if (operation == "config.list") {
        nlohmann::json data = nlohmann::json::object();
        data["current_user"] = context.config.current_user;
        data["current_bucket"] = context.config.current_bucket;
        data["max_upload_size"] = context.config.max_upload_size;
        data["size_unit"] = context.config.size_unit;
        data["language"] = context.config.language;
        data["path"] = relative_path_text(context.paths->root(), context.paths->config_file());

        // HTTP 管理接口的访问 token（data/user.json 里那个）。放在这里是因为
        // 否则用户没有任何办法拿到它——CLI 是这个数据根唯一的管理入口。
        UserStore store(*context.paths, context.config);
        if (const Result<UserRecord> user = store.find_by_name(context.config.current_user);
            ok(user)) {
            const UserRecord& record = std::get<UserRecord>(user);
            data["user_id"] = record.user_id;
            data["token"] = record.token;
            data["user_created_at"] = record.created_at;
            data["last_login_at"] = record.last_login_at;
        }
        return data;
    }

    if (operation == "config.set") {
        const Result<std::string> key = argument(args, 0, "配置项名称");
        if (!ok(key)) {
            return *error_of(key);
        }
        const Result<std::string> value = argument(args, 1, "配置值");
        if (!ok(value)) {
            return *error_of(value);
        }

        const std::string name = std::get<std::string>(key);
        if (name != "max_upload_size") {
            return make_error(ErrorCode::InvalidArgument,
                              "V1 只能改 max_upload_size（其余只读）：" + name);
        }

        const Result<std::uintmax_t> parsed = parse_size_value(std::get<std::string>(value));
        if (!ok(parsed)) {
            return *error_of(parsed);
        }
        const std::uintmax_t bytes = std::get<std::uintmax_t>(parsed);

        const std::uintmax_t previous = context.config.max_upload_size;
        context.config.max_upload_size = bytes;
        if (const Status saved = save_config(*context.paths, context.config); !ok(saved)) {
            context.config.max_upload_size = previous;  // 落盘失败就回滚内存里的值
            return *error_of(saved);
        }
        if (context.logger != nullptr) {
            context.logger->info("Config", "max_upload_size：" + std::to_string(previous) + " -> " +
                                               std::to_string(bytes));
        }

        nlohmann::json data = nlohmann::json::object();
        data["max_upload_size"] = bytes;
        data["previous"] = previous;
        data["message"] = "max_upload_size 已改为 " + std::to_string(bytes) + " 字节（" +
                          format_size(bytes) + "）";
        return data;
    }

    return make_error(ErrorCode::InvalidArgument, "未知的配置操作：" + operation);
}

// share.*：数据面（创建/查看/列出/撤销 + 下载记账）。
// **HTTP 下载端点还没做**（用户决定先做数据面）：register_download 已经把
// §50/§51 那套「全部检查通过才计数、且在业务锁内完成」实现好，端点将来只管调它。
Result<nlohmann::json> share_command(AppContext& context, const std::string& operation,
                                     const nlohmann::json& args) {
    ShareService shares(*context.paths, context.config, context.logger.get());

    const auto share_json = [](const ShareRecord& record) {
        nlohmann::json item = nlohmann::json::object();
        item["share_id"] = record.share_id;
        item["file_id"] = record.file_id;
        item["max_download_count"] = record.max_download_count;
        item["download_count"] = record.download_count;
        item["expire_time"] = record.expire_time.empty() ? nlohmann::json(nullptr)
                                                         : nlohmann::json(record.expire_time);
        item["is_valid"] = record.is_valid;
        return item;
    };
    const auto view_json = [&share_json](const ShareView& view) {
        nlohmann::json item = share_json(view.share);
        item["state"] = std::string(share_state_name(view.state));
        item["available"] = view.state == ShareState::Ok;
        item["message"] = view.message;
        if (!view.file.file_id.empty()) {
            item["file_name"] = view.file.file_name;
            item["size"] = view.file.size;
            item["bucket"] = view.file.bucket;
        }
        return item;
    };

    if (operation == "share.create") {
        const Result<std::string> file_id = argument(args, 0, "file_id");
        if (!ok(file_id)) {
            return *error_of(file_id);
        }
        const Result<ShareRecord> created = shares.create(std::get<std::string>(file_id));
        if (!ok(created)) {
            return *error_of(created);
        }
        const ShareRecord& record = std::get<ShareRecord>(created);
        nlohmann::json data = share_json(record);
        data["message"] = "分享已创建：" + record.share_id + "（" +
                          std::to_string(record.max_download_count) + " 次，到期 " +
                          record.expire_time + "）";
        return data;
    }

    if (operation == "share.get") {
        const Result<std::string> share_id = argument(args, 0, "share_id");
        if (!ok(share_id)) {
            return *error_of(share_id);
        }
        // 未知 id 是真错误（FMT-500）；「存在但过期 / 次数用尽 / 文件进回收站」如实返回。
        const Result<ShareView> view = shares.get(std::get<std::string>(share_id));
        if (!ok(view)) {
            return *error_of(view);
        }
        return view_json(std::get<ShareView>(view));
    }

    if (operation == "share.list") {
        const Result<std::string> file_id = argument(args, 0, "file_id");
        if (!ok(file_id)) {
            return *error_of(file_id);
        }
        const Result<std::vector<ShareView>> views = shares.list(std::get<std::string>(file_id));
        if (!ok(views)) {
            return *error_of(views);
        }

        nlohmann::json array = nlohmann::json::array();
        for (const ShareView& view : std::get<std::vector<ShareView>>(views)) {
            array.push_back(view_json(view));
        }
        nlohmann::json data = nlohmann::json::object();
        data["shares"] = std::move(array);
        data["count"] = data["shares"].size();
        data["file_id"] = std::get<std::string>(file_id);
        return data;
    }

    if (operation == "share.delete") {
        const Result<std::string> share_id = argument(args, 0, "share_id");
        if (!ok(share_id)) {
            return *error_of(share_id);
        }
        const Result<ShareRecord> removed = shares.remove(std::get<std::string>(share_id));
        if (!ok(removed)) {
            return *error_of(removed);
        }
        nlohmann::json data = share_json(std::get<ShareRecord>(removed));
        data["message"] = "分享已撤销：" + std::get<ShareRecord>(removed).share_id;
        return data;
    }

    if (operation == "share.download") {
        // 下载记账：检查与计数在业务锁内一次完成（§51）。
        // 现在只有管道调用它；将来的 HTTP 下载端点也走这一条。
        const Result<std::string> share_id = argument(args, 0, "share_id");
        if (!ok(share_id)) {
            return *error_of(share_id);
        }
        const Result<FileRecord> file = shares.register_download(std::get<std::string>(share_id));
        if (!ok(file)) {
            return *error_of(file);
        }
        const FileRecord& record = std::get<FileRecord>(file);
        nlohmann::json data = nlohmann::json::object();
        data["file_id"] = record.file_id;
        data["file_name"] = record.file_name;
        data["size"] = record.size;
        data["md5"] = record.md5;
        // 相对路径：公开的分享下载端点要按它去读文件（计数已经在上一步记好）
        FileService files(*context.paths, context.config, context.logger.get());
        if (const Result<std::filesystem::path> absolute = files.resolve_path(record);
            ok(absolute)) {
            data["path"] = relative_path_text(context.paths->root(),
                                             std::get<std::filesystem::path>(absolute));
        }
        data["message"] = "下载计数已记账：" + record.file_name;
        return data;
    }

    return make_error(ErrorCode::InvalidArgument, "未知的分享操作：" + operation);
}

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

    if (starts_with(operation, "config.")) {
        return config_command(context, operation, args);
    }

    if (starts_with(operation, "share.")) {
        return share_command(context, operation, args);
    }

    // 已经登记、还没实现的模块（只剩 server.*）。
    return make_error(ErrorCode::ServiceOperationFailed, "操作尚未实现：" + operation);
}

}  // namespace fmt::service
