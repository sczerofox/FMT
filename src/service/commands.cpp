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
#include "fmt/user/user_service.hpp"

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

// ---------------------------------------------------------------------------
// 对外的完整链接（本次改动（未提交））
//
// 分享链接与文件预览链接都要**无论 HTTP 服务有没有开**都给出完整地址：
// 地址来自数据根下的 config/server.json（host / port），不看 enabled——
// 用户要的是「这条链接该长什么样」，而不是「现在能不能连」。
// 协议固定 **http://**：服务端只监听明文 HTTP（server.json 里没有 TLS 配置，
// 也没有证书），要 https 得自己在前面挂反向代理；这里不编造 https 地址。
// ---------------------------------------------------------------------------
std::string base_url_of(const ServerConfig& server) {
    // host / port 缺失或非法时退回默认值，绝不拼出 "http://:4122/…" 这种废地址。
    std::string host = trim(server.host);
    if (host.empty()) {
        host = "localhost";
    }
    const bool port_ok = server.port > 0 && server.port <= 65535;
    const int port = port_ok ? server.port : 4122;
    return "http://" + host + ":" + std::to_string(port);
}

// 公开的分享下载链接（GET /api/share/<share_id>/download，不需要 token）。
std::string share_url_of(const AppContext& context, const std::string& share_id) {
    return base_url_of(context.server_config) + "/api/share/" + share_id + "/download";
}

// 文件预览链接（GET /api/file/<file_id>/preview，要 token）。
// 路径用 **file_id**，不用文件名：它全局唯一、只含 [0-9a-z-]，不用做百分号编码。
std::string preview_url_of(const AppContext& context, const std::string& file_id) {
    return base_url_of(context.server_config) + "/api/file/" + file_id + "/preview";
}

// 文件下载链接（GET /api/file/<file_id>/download，要 token）。
std::string download_url_of(const AppContext& context, const std::string& file_id) {
    return base_url_of(context.server_config) + "/api/file/" + file_id + "/download";
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
        // 桶级回退的两条路：目标不在 = 整目录搬回；目标已存在 = 合并进去（要确认）
        data["merge"] = check.merge;
        data["merge_target_exists"] = check.merge_target_exists;
        data["merge_files"] = check.merge_files;
        data["merge_bytes"] = check.merge_bytes;
        data["target_files"] = check.target_files;
        if (!check.conflicts.empty()) {
            data["conflicts"] = check.conflicts;
        }
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

        // 目标桶已存在时回退是**合并**：这是「拿回收站的数据往现有桶里加东西」，
        // 与永久删除同一口径——必须显式确认（CLI 在用户答 y 之后才置 force）。
        // 普通回退（目标不在）预检的 needs_confirm 是 false，不受这道门槛影响。
        bool confirmed = false;
        {
            const Result<TrashCheck> checked = trash.check_restore(value);
            if (!ok(checked)) {
                return *error_of(checked);
            }
            const TrashCheck& check = std::get<TrashCheck>(checked);
            if (!check.conflicts.empty()) {
                return make_error(ErrorCode::RestoreMergeConflict, check.message);
            }
            if (check.needs_confirm) {
                confirmed = args.value("force", false);
                if (!confirmed) {
                    return make_error(ErrorCode::ConfirmRequired,
                                      check.message + "；确认合并请加 force（CLI：--yes）");
                }
            }
        }

        const Result<TrashEntry> restored = trash.restore(value, confirmed);
        if (!ok(restored)) {
            return *error_of(restored);
        }
        const TrashEntry& entry = std::get<TrashEntry>(restored);

        nlohmann::json data = nlohmann::json::object();
        data["entry"] = entry_json(entry);
        if (entry.type == "file") {
            data["message"] = "文件已回退：" + entry.name;
        } else if (!entry.message.empty()) {
            // 目标 Bucket 已存在时这次回退是**合并**，结果由服务层写进 entry.message
            //（「已合并 N 个文件」「N 个留在回收站里」）——照实转述，别盖成「Bucket 已回退」。
            data["message"] = "Bucket「" + entry.name + "」：" + entry.message;
        } else {
            data["message"] = "Bucket 已回退：" + entry.name;
        }
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
        // 预览 / 下载链接（本次改动（未提交））：**无论 HTTP 服务有没有开**都给完整地址，
        // 方便直接粘进浏览器；预览只对图片与文本类有意义（其余类型下载仍可用）。
        data["preview_url"] = preview_url_of(context, found.file_id);
        data["download_url"] = download_url_of(context, found.file_id);
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

// 解析「时长」：`30s` / `45m` / `12h` / `7d`，或者纯秒数（`3600`）。大小写不敏感。
//
// 为什么接受这些写法：token 的有效期让用户在命令行里手敲秒数太反人类
//（`604800` 是 7 天），而 `7d` 一眼就懂。单位只取 s/m/h/d 四档，够用且不歧义。
Result<std::uint64_t> parse_duration_seconds(const std::string& text) {
    std::string value = to_lower(trim(text));
    if (value.empty()) {
        return make_error(ErrorCode::InvalidArgument, "时长不能为空（例如 7d / 12h / 3600）");
    }

    std::uint64_t multiplier = 1;
    for (const auto& unit : {std::pair<std::string, std::uint64_t>{"d", 24ULL * 60 * 60},
                             {"h", 60ULL * 60}, {"m", 60ULL}, {"s", 1ULL}}) {
        if (ends_with(value, unit.first)) {
            multiplier = unit.second;
            value = trim(value.substr(0, value.size() - unit.first.size()));
            break;
        }
    }
    if (value.empty()) {
        return make_error(ErrorCode::InvalidArgument, "时长缺少数字：" + text);
    }

    std::uint64_t number = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return make_error(ErrorCode::InvalidArgument,
                              "时长只能是数字（可带 s/m/h/d）：" + text);
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (number > (UINT64_MAX - digit) / 10) {
            return make_error(ErrorCode::InvalidArgument, "时长超出可表示范围：" + text);
        }
        number = number * 10 + digit;
    }
    if (number > UINT64_MAX / multiplier) {
        return make_error(ErrorCode::InvalidArgument, "时长超出可表示范围：" + text);
    }
    return number * multiplier;
}

// 解析布尔写法（本次改动（未提交））：true/false、1/0、yes/no、on/off，大小写不敏感。
Result<bool> parse_bool_value(const std::string& text) {
    const std::string value = to_lower(trim(text));
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return Result<bool>(true);
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return Result<bool>(false);
    }
    return make_error(ErrorCode::InvalidArgument,
                      "布尔值只能是 true / false（也接受 1 / 0、yes / no、on / off）：" + text);
}

// 解析端口：1..65535 的十进制整数。
Result<int> parse_port_value(const std::string& text) {
    const std::string value = trim(text);
    if (value.empty()) {
        return make_error(ErrorCode::InvalidArgument, "端口不能为空（1-65535）");
    }
    int port = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return make_error(ErrorCode::InvalidArgument, "端口只能是数字（1-65535）：" + text);
        }
        port = port * 10 + (ch - '0');
        if (port > 65535) {
            break;
        }
    }
    if (port <= 0 || port > 65535) {
        return make_error(ErrorCode::InvalidArgument, "端口超出范围（1-65535）：" + text);
    }
    return port;
}

// config.*：可改 max_upload_size（config.json）与 server_enabled / server_host /
// server_port（config/server.json，本次改动（未提交））；其余只读。
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

        // config/server.json 的公共字段（本次改动（未提交））：统一加 `server_` 前缀，
        // 免得和 config.json 的字段撞名（两边都有 version）。
        // 这些是「网络服务」的开关与地址，CLI 打成「网络服务 server_enabled：…」这样一行。
        // **不含任何凭证**：token / 口令哈希都在 data/user.json，只在下面单独给出 token。
        data["server_enabled"] = context.server_config.enabled;
        data["server_host"] = context.server_config.host;
        data["server_port"] = context.server_config.port;
        data["server_path"] =
            relative_path_text(context.paths->root(), context.paths->server_file());

        // HTTP 管理接口的访问 token（data/user.json 里那个）。放在这里是因为
        // 否则用户没有任何办法拿到它——CLI 是这个数据根唯一的管理入口。
        UserStore store(*context.paths, context.config);
        if (const Result<UserRecord> user = store.find_by_name(context.config.current_user);
            ok(user)) {
            const UserRecord& record = std::get<UserRecord>(user);
            data["user_id"] = record.user_id;
            data["token"] = record.token;
            // token 的时效：CLI 要能把「什么时候过期」一起打出来，
            // 否则用户看到 token 却不知道它还能用多久。
            data["token_issued_at"] = record.token_issued_at;
            data["token_expires_at"] = record.token_expires_at;
            data["token_expired"] = token_expired(record);
            data["token_expiry_text"] = token_expiry_text(record);
            data["user_created_at"] = record.created_at;
            data["last_login_at"] = record.last_login_at;
        }
        // 新签发 token 时用的有效期（秒）：0 是不允许的，见 Config::token_ttl_seconds。
        data["token_ttl_seconds"] = context.config.token_ttl_seconds;
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
        const std::string text = std::get<std::string>(value);
        nlohmann::json data = nlohmann::json::object();

        // config/server.json 的三项（本次改动（未提交））：网络服务开关、监听地址与端口。
        // 它们的共同点是**改完必须重启服务才生效**（server.json 只在服务启动时读一次），
        // 所以这里只负责把文件改对、并把「要重启」写进返回值；真正的 reinstall 由 CLI 做
        // ——服务进程去提权装自己是在踩自己的脚（第 13.8 节）。
        if (name == "server_enabled" || name == "server_host" || name == "server_port") {
            const ServerConfig previous = context.server_config;
            ServerConfig updated = previous;

            if (name == "server_enabled") {
                const Result<bool> parsed = parse_bool_value(text);
                if (!ok(parsed)) {
                    return *error_of(parsed);
                }
                updated.enabled = std::get<bool>(parsed);
                data["server_enabled"] = updated.enabled;
            } else if (name == "server_host") {
                if (trim(text).empty()) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "server_host 不能为空（例如 localhost / 127.0.0.1）");
                }
                updated.host = trim(text);
                data["server_host"] = updated.host;
            } else {
                const Result<int> parsed = parse_port_value(text);
                if (!ok(parsed)) {
                    return *error_of(parsed);
                }
                updated.port = std::get<int>(parsed);
                data["server_port"] = updated.port;
            }

            if (const Status saved = save_server_config(*context.paths, updated); !ok(saved)) {
                return *error_of(saved);  // 留着内存里的旧值：下一轮读盘会覆盖回来
            }
            context.server_config = updated;
            if (context.logger != nullptr) {
                context.logger->info("Config", "server.json 已修改：" + name + " -> " +
                                                   text + "（需要重启服务才生效）");
            }

            // 「谁来重启」不在服务侧决定：服务自己提权重装会把正在跑的业务打断。
            // CLI 收到 restart_required 后会判断能不能直接 reinstall，否则提示用户执行。
            data["restart_required"] = true;
            data["server_path"] =
                relative_path_text(context.paths->root(), context.paths->server_file());
            if (name == "server_enabled") {
                data["message"] = "网络服务已" + std::string(updated.enabled ? "开启" : "关闭") +
                                  "（config/server.json 已写入）；需要重启服务才生效";
            } else {
                data["message"] = name + " 已改为 " + text + "（config/server.json 已写入）；"
                                  "需要重启服务才生效";
            }
            return data;
        }

        // 新签发 token 的有效期（秒）。改它只影响**下一次签发**，不动已经发出的
        // token（有效期在签发时就写死在 user.json 里了）——行为可预测，
        // 也不会让正在用的客户端突然失效。
        if (name == "token_ttl") {
            const Result<std::uint64_t> parsed = parse_duration_seconds(text);
            if (!ok(parsed)) {
                return *error_of(parsed);
            }
            const std::uint64_t seconds = std::get<std::uint64_t>(parsed);
            if (seconds == 0) {
                // 允许 0 就等于给「永不过期」留后门，而用户的要求正是「过时不能用」。
                return make_error(ErrorCode::InvalidArgument,
                                  "token_ttl 必须是正数（例如 7d / 12h / 3600）；"
                                  "不允许 0（token 必须有时效）");
            }
            const std::uint64_t previous = context.config.token_ttl_seconds;
            context.config.token_ttl_seconds = seconds;
            if (const Status saved = save_config(*context.paths, context.config); !ok(saved)) {
                context.config.token_ttl_seconds = previous;
                return *error_of(saved);
            }
            if (context.logger != nullptr) {
                context.logger->info("Config", "token_ttl_seconds：" + std::to_string(previous) +
                                                   " -> " + std::to_string(seconds));
            }
            data["token_ttl_seconds"] = seconds;
            data["previous"] = previous;
            data["message"] = "token_ttl 已改为 " + std::to_string(seconds) +
                              " 秒（下次签发 token 时生效；已有 token 的有效期不变）";
            return data;
        }

        if (name != "max_upload_size") {
            return make_error(ErrorCode::InvalidArgument,
                              "只能改 max_upload_size / token_ttl / server_enabled / server_host / "
                              "server_port（其余只读）：" + name);
        }

        const Result<std::uintmax_t> parsed = parse_size_value(text);
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

        data["max_upload_size"] = bytes;
        data["previous"] = previous;
        data["message"] = "max_upload_size 已改为 " + std::to_string(bytes) + " 字节（" +
                          format_size(bytes) + "）";
        return data;
    }

    return make_error(ErrorCode::InvalidArgument, "未知的配置操作：" + operation);
}

// share.*：数据面（创建/查看/列出/撤销 + 下载记账）。
// 下载也走这条：HTTP 的 GET /api/share/<share_id>/download（**公开、不要 token**）
// 先调 share.download 记账，全部检查通过才把字节流出去——§50/§51 的「先记账再放行」
// 就落在 register_download 里（端点只负责记账完之后去读文件、回内容）。
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
        // 「为什么不能下载」的两条直接给成布尔（本次改动（未提交））：调用方不用自己去比
        // 时间、也不用自己算剩余次数。state 里其实已经含了，但这两个是**独立维度**：
        // 一条分享可以既过期、次数又用满（§49 只报先命中的那个状态）。
        item["expired"] = (view.state == ShareState::Expired);
        item["limit_reached"] = (view.state == ShareState::LimitReached);
        item["downloads_left"] =
            std::max(0, view.share.max_download_count - view.share.download_count);
        if (!view.file.file_id.empty()) {
            item["file_name"] = view.file.file_name;
            item["size"] = view.file.size;
            item["bucket"] = view.file.bucket;
            item["file_in_trash"] = view.file.is_trash;
        }
        return item;
    };

    if (operation == "share.create") {
        const Result<std::string> key = argument(args, 0, "file_id 或文件名");
        if (!ok(key)) {
            return *error_of(key);
        }
        const Result<ShareRecord> created = shares.create(std::get<std::string>(key));
        if (!ok(created)) {
            return *error_of(created);
        }
        const ShareRecord& record = std::get<ShareRecord>(created);
        nlohmann::json data = share_json(record);
        // 完整分享链接（本次改动（未提交））：**无论 HTTP 服务有没有开**都给出来，
        // 地址取自 config/server.json 的 host / port（`share_id` 本身就是凭证）。
        data["url"] = share_url_of(context, record.share_id);
        data["share_url"] = data["url"];
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
        nlohmann::json data = view_json(std::get<ShareView>(view));
        data["url"] = share_url_of(context, std::get<ShareView>(view).share.share_id);
        data["share_url"] = data["url"];
        return data;
    }

    if (operation == "share.list") {
        // **参数可选**（本次改动（未提交））：不给就列全部 share（`share list` 一行看遍
        // 谁过期、谁次数用尽），给了就按那个文件（file_id 或文件名，与 create 同一套定位）。
        std::string key;
        const auto argv = args.find("argv");
        if (argv != args.end() && argv->is_array() && !argv->empty()) {
            const Result<std::string> wanted = argument(args, 0, "file_id 或文件名");
            if (!ok(wanted)) {
                return *error_of(wanted);
            }
            key = std::get<std::string>(wanted);
        }

        std::string file_id;
        if (!key.empty()) {
            const Result<FileRecord> resolved = shares.resolve(key);
            if (!ok(resolved)) {
                return *error_of(resolved);
            }
            file_id = std::get<FileRecord>(resolved).file_id;
        }

        const Result<std::vector<ShareView>> views = shares.list(key);
        if (!ok(views)) {
            return *error_of(views);
        }

        nlohmann::json array = nlohmann::json::array();
        for (const ShareView& view : std::get<std::vector<ShareView>>(views)) {
            nlohmann::json item = view_json(view);
            item["url"] = share_url_of(context, view.share.share_id);
            item["share_url"] = item["url"];
            array.push_back(std::move(item));
        }
        nlohmann::json data = nlohmann::json::object();
        data["shares"] = std::move(array);
        data["count"] = data["shares"].size();
        // 过滤时回显那个文件的 file_id；不带参数列全部时给一个字段说明这是全量列表，
        // 顺便带上当前 Bucket（列表里每条都带自己的 bucket，方便区分）。
        data["scope"] = key.empty() ? "all" : "file";
        // 当前 Bucket 两种模式都给：列全部时用户靠它分辨「这条是不是当前桶的文件」，
        // 过滤单个文件时也能顺手看出它属于哪个桶。
        data["current_bucket"] = context.config.current_bucket;
        if (!key.empty()) {
            data["file_id"] = file_id;
        }
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
        // 两个入口都走它：管道的 `share download`，以及 HTTP 的公开端点
        // GET /api/share/<share_id>/download（先记账再放行——计数写不进去就不放行）。
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

// 校验「旧口令的摘要」：CLI 侧算好送过来，服务只做常量时间比对。
//
// 明文不参与：服务只存单向摘要（见 user.hpp 的 verify_password_digest 说明）。
Result<UserRecord> take_verified_user(AppContext& context, const nlohmann::json& args,
                                      const char* operation) {
    const std::string digest = text_arg(args, "current_digest", std::string{});
    if (digest.size() != 64) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(operation) + " 缺少当前口令的摘要");
    }
    user::UserService users(*context.paths, context.config);
    return users.verify_digest(digest);
}

// 用户域：账号的创建 / 查看 / 改密 / 删除前的口令校验。
//
// **口令明文从不进这里**：CLI 用 `password_digest(用户名, 口令)` 自己算摘要，
// 只把摘要发过来。`user.delete` 在这里只做「口令对不对」这一步，
// 真正删目录由 CLI 本地完成（服务正在跑、持有 log/ 句柄，让它删自己的数据根是踩自己的脚）。
Result<nlohmann::json> user_command(AppContext& context, const std::string& operation,
                                    const nlohmann::json& args) {
    user::UserService users(*context.paths, context.config);

    if (operation == "user.create") {
        const std::string username = text_arg(args, "username", std::string{});
        const std::string digest = text_arg(args, "password_digest", std::string{});
        if (username.empty()) {
            return make_error(ErrorCode::InvalidArgument, "用法：user create <用户名>");
        }
        const Result<UserRecord> created = users.create(username, digest);
        if (!ok(created)) {
            return *error_of(created);
        }
        const UserRecord& record = std::get<UserRecord>(created);

        nlohmann::json data = nlohmann::json::object();
        // 口径与 HTTP 的 /api/config 一致：这里**不给 token**（要它用 `config list`）。
        data["username"] = record.username;
        data["user_id"] = record.user_id;
        data["created_at"] = record.created_at;
        data["path"] = "data/user.json";
        data["password_changed"] = false;
        data["message"] = "账号已创建：" + record.username +
                          "（密码只能修改一次；再次修改需要 user delete 后重建）";
        return data;
    }

    if (operation == "user.get") {
        return users.describe();
    }

    // 口令校验：CLI 用 user.salt 算好摘要送过来，这里只做常量时间比对。
    // 登录（`fmt -u <用户名> -p`）与「删除用户前先验口令」都走它。
    if (operation == "user.verify") {
        const Result<UserRecord> verified = take_verified_user(context, args, "user verify");
        if (!ok(verified)) {
            return *error_of(verified);
        }
        nlohmann::json data = nlohmann::json::object();
        data["username"] = std::get<UserRecord>(verified).username;
        data["user_id"] = std::get<UserRecord>(verified).user_id;
        data["path"] = "data/user.json";
        data["message"] = "登录成功";
        return data;
    }

    if (operation == "user.salt") {
        const Result<std::string> salt = users.password_salt();
        if (!ok(salt)) {
            return *error_of(salt);
        }
        nlohmann::json data = nlohmann::json::object();
        data["salt"] = std::get<std::string>(salt);
        return data;
    }

    // 换发 token（`user token renew`）：凭**密码摘要**，不凭旧 token。
    // 这是 token 过期后唯一的续期路径——否则过期就只能删号重建。
    if (operation == "user.token.renew") {
        const std::string digest = text_arg(args, "current_digest", std::string{});
        const Result<nlohmann::json> renewed = users.renew_token(digest);
        if (!ok(renewed)) {
            return *error_of(renewed);
        }
        return std::get<nlohmann::json>(renewed);
    }

    if (operation == "user.update") {
        const std::string current = text_arg(args, "current_digest", std::string{});
        const std::string fresh = text_arg(args, "password_digest", std::string{});
        const Result<std::string> changed = users.update_password(current, fresh);
        if (!ok(changed)) {
            return *error_of(changed);
        }
        nlohmann::json data = nlohmann::json::object();
        data["message"] = std::get<std::string>(changed);
        data["password_changed"] = true;
        return data;
    }

    if (operation == "user.delete") {
        const Result<UserRecord> proved = take_verified_user(context, args, "user delete");
        if (!ok(proved)) {
            return *error_of(proved);
        }
        nlohmann::json data = nlohmann::json::object();
        data["username"] = std::get<UserRecord>(proved).username;
        data["path"] = "data/user.json";
        data["message"] = "口令已确认，可以删除数据根";
        return data;
    }

    return make_error(ErrorCode::InvalidArgument, "未知的用户操作：" + operation);
}

bool is_known_business(const std::string& operation) {
    for (const std::string_view prefix :
         {"bucket.", "file.", "share.", "trash.", "config.", "server.", "user."}) {
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

    if (starts_with(operation, "user.")) {
        return user_command(context, operation, args);
    }

    // 已经登记、还没实现的模块（只剩 server.*）。
    return make_error(ErrorCode::ServiceOperationFailed, "操作尚未实现：" + operation);
}

}  // namespace fmt::service
