// 文件业务（开发文档第 31～43 节）
//
// **上传是两段式**，因为下载/复制可能几十秒到几分钟：
//   ① prepare_upload()  在**锁外**跑：下载或复制到 `<数据根>/temp/`，边写边算 MD5；
//   ② commit_upload()   在**锁内**跑：去重、重名、file_id、移动到仓库、写 file.json。
// 长任务不持业务锁，桶命令与浏览器请求才不会被一次上传整段卡住（技术文档并发一节）。
//
// 存储日期**由 file_id 推出**：`fmt-YYYYMMDD-N` 的日期片段就是入库日期，
// 所以 file.json 里不需要再存路径或时间（第 7.6 节：记录只存层级信息）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/path_manager.hpp"

namespace fmt {

// file.json 的一条记录（开发文档第 14 节）。
struct FileRecord {
    std::string file_id;
    std::string user;
    std::string bucket;
    std::string file_name;
    std::string extension;     // 小写，含点；没有扩展名时为空
    std::string file_type;     // image / text / video / archive / other
    std::uintmax_t size = 0;   // 字节
    std::string md5;           // 32 位小写十六进制
    bool is_trash = false;
    std::string trash_reason;  // "bucket" / "file" / 空
};

// 「要删的到底是哪一个、有没有要先说清楚的情况」——**预检，零副作用**。
//
// CLI 用它实现「先检查 → 说清楚冲突的具体对象 → 再让用户确认」：
//   目标唯一但在别的 Bucket  -> other_bucket（需要确认，说清是哪两个桶）
//   名字与 file_id 撞车      -> ambiguous（**不能靠 y/N 解决**：y 无法表达删哪一个），
//                              候选放在 candidates 里，让用户改用 file_id
struct FileDeleteCheck {
    std::string file_id;
    std::string file_name;
    std::string bucket;          // 目标所属 Bucket
    std::string current_bucket;  // 当前 Bucket
    std::string path;            // 能定位到时给出（相对数据根、正斜杠）
    bool other_bucket = false;
    bool ambiguous = false;
    std::vector<FileRecord> candidates;  // 歧义时的候选（各带 file_id/file_name/bucket）
    std::string message;                 // 给用户看的一句话
};

struct PreparedUpload {
    std::filesystem::path temp_path;
    std::string file_name;
    std::uintmax_t size = 0;
    std::string md5;
};

// 从来源推断文件名：URL 的最后一段（百分号解码）或本地路径的文件名。
// 推断不出来返回空串——这时要用户显式给名字。
std::string file_name_from_source(std::string_view source);

// 小写扩展名（含点）；无扩展名返回空串。
std::string extension_of(const std::string& file_name);

// 上传第一阶段。任何失败都会删掉临时文件（第 35 节）。
//   source   : `http://` 开头的 URL，或本地文件路径
//   name     : 用户指定的文件名；为空则由 source 推断
//   size_limit: 超过就中止（第 8 步：边写边判，不是下完再看）
Result<PreparedUpload> prepare_upload(const PathManager& paths, const std::string& source,
                                      const std::string& name, std::uintmax_t size_limit,
                                      Logger* logger);

class FileService {
public:
    FileService(const PathManager& paths, Config& config, Logger* logger);

    // 上传第二阶段：快、必须持锁。成功即已入库并写进 file.json。
    // **失败时会把临时文件删掉**（第 35 节）：接手之后临时文件的生命周期归它管。
    Result<FileRecord> commit_upload(PreparedUpload& prepared);

    // 当前用户 + 当前 Bucket + 正常文件（第 41 节）。
    Result<std::vector<FileRecord>> list();
    // file_id 全局唯一（第 42 节）。
    Result<FileRecord> get_by_id(std::string_view file_id);
    // 按名字查：当前用户 + 正常文件。
    Result<FileRecord> get_by_name(std::string_view file_name);
    // 软删除进回收站，file_id 不变（第 43 节）。
    // 参数与 file get 一致：先当 file_id 查，再当文件名查（当前用户 + 正常文件）。
    Result<FileRecord> remove(std::string_view file_id_or_name);

    // 删除前的预检：只读，不改任何东西（跨桶确认与歧义检测都靠它）。
    Result<FileDeleteCheck> check_remove(std::string_view file_id_or_name) const;

    // 仓库里的实际路径；回收站里的实际路径。
    Result<std::filesystem::path> resolve_path(const FileRecord& record) const;
    Result<std::filesystem::path> trash_path_of(const FileRecord& record) const;

private:
    Result<std::vector<FileRecord>> load_records() const;
    Status save_records(const std::vector<FileRecord>& records) const;
    std::string next_file_id(const std::vector<FileRecord>& records) const;
    // 按 file_id 或文件名定位一条记录，并给出**准确**的失败原因
    // （「名字存在但已在回收站」不能报成「文件不存在」）。
    Result<std::size_t> locate_record(const std::vector<FileRecord>& records,
                                      std::string_view key) const;
    // 两个维度各自命中哪一条（预检与定位共用，保证判定只有一份）。
    struct RecordMatch {
        bool has_id = false;
        bool has_name = false;
        std::size_t by_id = 0;
        std::size_t by_name = 0;
    };
    RecordMatch match_record(const std::vector<FileRecord>& records, std::string_view key) const;
    std::filesystem::path bucket_path() const;

    const PathManager& paths_;
    Config& config_;
    Logger* logger_ = nullptr;
};

}  // namespace fmt
