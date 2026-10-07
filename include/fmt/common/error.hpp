// FMT 统一错误系统
//
// 两条规则（见 docx/FMT 项目架构.md 附录 A）：
//   1. 错误码稳定，错误消息可修改；
//   2. 编号一旦发布，不复用、不修改语义，新增错误只能追加新编号。
//
// 错误码 FMT-NNN 与退出码是两个层次：错误码定位具体原因，退出码给脚本分类。
// 枚举值就是 NNN，因此 code_string() 与 code_from_string() 不需要额外的映射表。
#pragma once

#include <string>
#include <string_view>
#include <variant>

namespace fmt {

// 错误码。枚举值等于 FMT-NNN 中的 NNN。
enum class ErrorCode : int {
    Ok = 0,

    // ---- FMT-0xx 通用、JSON、路径 ----
    InvalidArgument = 1,           // 参数错误               退出码 2
    FileNotFound = 2,              // 文件不存在             退出码 3
    FileAlreadyExists = 3,         // 文件已存在             退出码 4
    PermissionDenied = 4,          // 权限不足               退出码 5
    IoError = 5,                   // 磁盘 / IO 错误         退出码 1
    JsonParseError = 6,            // JSON 格式错误          退出码 6
    JsonWriteError = 7,            // JSON 写入失败          退出码 1
    ConfigError = 8,               // 配置错误               退出码 7
    StorageError = 9,              // 存储操作失败           退出码 1
    Md5Error = 10,                 // MD5 计算失败           退出码 1
    JsonUnsupportedVersion = 11,   // JSON 版本不受支持      退出码 6
    PathTooLong = 12,              // 路径或文件名超长       退出码 2
    DirectoryCreateFailed = 13,    // 目录创建失败           退出码 1
    PathEscape = 14,               // 路径穿越               退出码 2
    ConsistencyError = 15,         // 数据一致性异常         退出码 6
    ConfirmRequired = 16,          // 该操作需要显式确认     退出码 2

    // ---- FMT-1xx 文件名校验 ----
    FileNameEmpty = 100,           // 文件名为空             退出码 2
    FileNameInvalidChar = 101,     // 含 Windows 非法字符    退出码 2
    FileNameSeparator = 102,       // 含路径分隔符           退出码 2
    FileNameReserved = 103,        // Windows 保留设备名     退出码 2
    FileNameTooLong = 104,         // 文件名超长             退出码 2
    FileNameConflict = 105,        // 同用户正常文件重名     退出码 4
    FileNameLikeFileId = 106,      // 名字与 file_id 同形    退出码 2

    // ---- FMT-2xx Bucket ----
    BucketNotFound = 200,          // Bucket 不存在          退出码 3
    BucketAlreadyExists = 201,     // Bucket 已存在          退出码 4
    BucketNameInvalid = 202,       // Bucket 名称非法        退出码 2
    BucketInUse = 203,             // Bucket 仍被引用        退出码 4

    // ---- FMT-3xx 上传与下载 ----
    UrlInvalid = 300,              // URL 非法或协议不支持   退出码 2
    DownloadFailed = 301,          // 下载失败               退出码 1
    DownloadTimeout = 302,         // 下载超时               退出码 1
    SizeLimitExceeded = 303,       // 超过最大上传大小       退出码 2
    Md5Duplicate = 304,            // MD5 已存在，文件重复   退出码 4
    NoCurrentBucket = 305,         // 未设置当前 Bucket      退出码 3

    // ---- FMT-4xx Trash ----
    TrashEntryNotFound = 400,      // Trash 记录不存在       退出码 3
    RestoreConflict = 401,         // 恢复目标已存在同名文件 退出码 4
    RestoreBucketMissing = 402,    // 原 Bucket 已永久删除   退出码 3

    // ---- FMT-5xx Share ----
    ShareNotFound = 500,           // Share 不存在           退出码 3
    ShareExpired = 501,            // Share 已过期           退出码 5
    ShareDownloadLimitReached = 502,  // 下载次数耗尽        退出码 5
    ShareFileUnavailable = 503,    // 关联文件不可用         退出码 5

    // ---- FMT-6xx Service ----
    ServiceAlreadyInstalled = 600, // 服务已安装             退出码 8
    ServiceNotInstalled = 601,     // 服务未安装             退出码 8
    ServiceOperationFailed = 602,  // 服务操作失败           退出码 8
    AdminRequired = 603,           // 需要管理员权限         退出码 5
    NoCurrentUser = 604,           // 未设置当前用户         退出码 7

    // ---- FMT-7xx HTTP ----
    HttpRequestInvalid = 700,      // HTTP 请求参数错误      退出码 2
    PreviewUnsupported = 701,      // 该文件类型不支持预览   退出码 2
};

// 错误值：错误码 + 可修改的消息。
struct Error {
    ErrorCode code{ErrorCode::Ok};
    std::string message;
};

// 有返回值的操作。成功值与错误值互斥，不使用「bool success + T value + Error」
// 这种允许误读的结构。
template <typename T>
using Result = std::variant<T, Error>;

// 无返回值的操作（delete / save / move 等）。
using Status = std::variant<std::monostate, Error>;

// 枚举名，例如 "PermissionDenied"；未知编号返回 "Unknown"。
std::string_view code_name(ErrorCode code);

// 错误码字符串，例如 "FMT-004"；ErrorCode::Ok 返回 "FMT-000"。
std::string code_string(ErrorCode code);

// 从 "FMT-004" 或 "4" 还原错误码（跨进程、跨日志还原用）。
// 无法识别时返回 ErrorCode::InvalidArgument，并把 ok 置为 false。
ErrorCode code_from_string(std::string_view text, bool* ok = nullptr);

// 附录 A 的 ErrorCode -> 退出码映射；未映射的编号默认返回 1（通用错误）。
int exit_code(ErrorCode code);

// 默认的中文消息，可被 Error::message 覆盖。
std::string_view default_message(ErrorCode code);

Error make_error(ErrorCode code);
Error make_error(ErrorCode code, std::string message);

// ---- 便捷判断 ----
inline bool ok(const Status& status) {
    return std::holds_alternative<std::monostate>(status);
}

template <typename T>
inline bool ok(const Result<T>& result) {
    return std::holds_alternative<T>(result);
}

inline const Error* error_of(const Status& status) {
    return std::get_if<Error>(&status);
}

template <typename T>
inline const Error* error_of(const Result<T>& result) {
    return std::get_if<Error>(&result);
}

}  // namespace fmt
