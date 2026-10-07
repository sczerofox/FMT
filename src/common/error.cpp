#include "fmt/common/error.hpp"

#include <cstdio>
#include <cstdlib>

namespace fmt {
namespace {

struct Entry {
    ErrorCode code;
    std::string_view name;
    int exit_code;
    std::string_view message;
};

// 唯一的事实来源：错误码清单（docx/FMT 项目架构.md 附录 A）。
// 新增错误只能往这里追加，不改动已有行的编号与语义。
constexpr Entry kTable[] = {
    // ---- FMT-0xx 通用、JSON、路径 ----
    {ErrorCode::InvalidArgument, "InvalidArgument", 2, "参数错误"},
    {ErrorCode::ConfirmRequired, "ConfirmRequired", 2, "该操作需要显式确认（force）"},
    {ErrorCode::FileNotFound, "FileNotFound", 3, "文件不存在"},
    {ErrorCode::FileAlreadyExists, "FileAlreadyExists", 4, "文件已存在"},
    {ErrorCode::PermissionDenied, "PermissionDenied", 5, "权限不足"},
    {ErrorCode::IoError, "IoError", 1, "磁盘或 IO 错误"},
    {ErrorCode::JsonParseError, "JsonParseError", 6, "JSON 格式错误"},
    {ErrorCode::JsonWriteError, "JsonWriteError", 1, "JSON 写入失败"},
    {ErrorCode::ConfigError, "ConfigError", 7, "配置错误"},
    {ErrorCode::StorageError, "StorageError", 1, "存储操作失败"},
    {ErrorCode::Md5Error, "Md5Error", 1, "MD5 计算失败"},
    {ErrorCode::JsonUnsupportedVersion, "JsonUnsupportedVersion", 6, "JSON 数据版本不受支持"},
    {ErrorCode::PathTooLong, "PathTooLong", 2, "路径或文件名超长"},
    {ErrorCode::DirectoryCreateFailed, "DirectoryCreateFailed", 1, "目录创建失败"},
    {ErrorCode::PathEscape, "PathEscape", 2, "路径穿越"},
    {ErrorCode::ConsistencyError, "ConsistencyError", 6, "数据一致性异常"},

    // ---- FMT-1xx 文件名校验 ----
    {ErrorCode::FileNameEmpty, "FileNameEmpty", 2, "文件名为空"},
    {ErrorCode::FileNameInvalidChar, "FileNameInvalidChar", 2, "文件名含 Windows 非法字符"},
    {ErrorCode::FileNameSeparator, "FileNameSeparator", 2, "文件名含路径分隔符"},
    {ErrorCode::FileNameReserved, "FileNameReserved", 2, "文件名是 Windows 保留设备名"},
    {ErrorCode::FileNameTooLong, "FileNameTooLong", 2, "文件名超长"},
    {ErrorCode::FileNameConflict, "FileNameConflict", 4, "同用户下已存在同名正常文件"},
    {ErrorCode::FileNameLikeFileId, "FileNameLikeFileId", 2,
     "文件名与文件标识同形（fmt-YYYYMMDD-N），会与 file_id 混淆"},

    // ---- FMT-2xx Bucket ----
    {ErrorCode::BucketNotFound, "BucketNotFound", 3, "Bucket 不存在"},
    {ErrorCode::BucketAlreadyExists, "BucketAlreadyExists", 4, "Bucket 已存在"},
    {ErrorCode::BucketNameInvalid, "BucketNameInvalid", 2, "Bucket 名称非法"},
    {ErrorCode::BucketInUse, "BucketInUse", 4, "Bucket 仍被引用，不能删除"},

    // ---- FMT-3xx 上传与下载 ----
    {ErrorCode::UrlInvalid, "UrlInvalid", 2, "URL 非法或协议不被支持"},
    {ErrorCode::DownloadFailed, "DownloadFailed", 1, "下载失败"},
    {ErrorCode::DownloadTimeout, "DownloadTimeout", 1, "下载超时"},
    {ErrorCode::SizeLimitExceeded, "SizeLimitExceeded", 2, "超过最大上传大小"},
    {ErrorCode::Md5Duplicate, "Md5Duplicate", 4, "文件内容已存在（MD5 重复）"},
    {ErrorCode::NoCurrentBucket, "NoCurrentBucket", 3, "未设置当前 Bucket"},

    // ---- FMT-4xx Trash ----
    {ErrorCode::TrashEntryNotFound, "TrashEntryNotFound", 3, "回收站记录不存在"},
    {ErrorCode::RestoreConflict, "RestoreConflict", 4, "恢复目标已存在同名文件"},
    {ErrorCode::RestoreBucketMissing, "RestoreBucketMissing", 3, "原 Bucket 已永久删除"},

    // ---- FMT-5xx Share ----
    {ErrorCode::ShareNotFound, "ShareNotFound", 3, "分享不存在"},
    {ErrorCode::ShareExpired, "ShareExpired", 5, "分享已过期"},
    {ErrorCode::ShareDownloadLimitReached, "ShareDownloadLimitReached", 5, "分享下载次数已耗尽"},
    {ErrorCode::ShareFileUnavailable, "ShareFileUnavailable", 5, "关联文件不可用"},

    // ---- FMT-6xx Service ----
    {ErrorCode::ServiceAlreadyInstalled, "ServiceAlreadyInstalled", 8, "服务已安装"},
    {ErrorCode::ServiceNotInstalled, "ServiceNotInstalled", 8, "服务未安装"},
    {ErrorCode::ServiceOperationFailed, "ServiceOperationFailed", 8, "服务操作失败"},
    {ErrorCode::AdminRequired, "AdminRequired", 5, "需要管理员权限"},
    {ErrorCode::NoCurrentUser, "NoCurrentUser", 7, "未设置当前用户"},

    // ---- FMT-7xx HTTP ----
    {ErrorCode::HttpRequestInvalid, "HttpRequestInvalid", 2, "HTTP 请求参数错误"},
    {ErrorCode::PreviewUnsupported, "PreviewUnsupported", 2, "该文件类型不支持预览"},
};

const Entry* find(ErrorCode code) {
    for (const Entry& entry : kTable) {
        if (entry.code == code) {
            return &entry;
        }
    }
    return nullptr;
}

const Entry* find(int number) {
    for (const Entry& entry : kTable) {
        if (static_cast<int>(entry.code) == number) {
            return &entry;
        }
    }
    return nullptr;
}

const Entry* find(std::string_view name) {
    for (const Entry& entry : kTable) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

std::string_view code_name(ErrorCode code) {
    if (code == ErrorCode::Ok) {
        return "Ok";
    }
    const Entry* entry = find(code);
    return entry != nullptr ? entry->name : std::string_view{"Unknown"};
}

std::string code_string(ErrorCode code) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "FMT-%03d", static_cast<int>(code));
    return std::string(buffer);
}

ErrorCode code_from_string(std::string_view text, bool* ok) {
    auto succeed = [ok](ErrorCode code) {
        if (ok != nullptr) {
            *ok = true;
        }
        return code;
    };
    auto fail = [ok]() {
        if (ok != nullptr) {
            *ok = false;
        }
        return ErrorCode::InvalidArgument;
    };

    // 去掉前缀 "FMT-"（大小写不敏感）。
    if (text.size() > 4) {
        const std::string_view prefix = text.substr(0, 4);
        if ((prefix[0] == 'F' || prefix[0] == 'f') && (prefix[1] == 'M' || prefix[1] == 'm') &&
            (prefix[2] == 'T' || prefix[2] == 't') && prefix[3] == '-') {
            text.remove_prefix(4);
        }
    }
    if (text.empty()) {
        return fail();
    }

    // 全数字：按编号查找。
    bool digits_only = true;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            digits_only = false;
            break;
        }
    }
    if (digits_only) {
        const int number = std::atoi(std::string(text).c_str());
        if (number == 0) {
            return succeed(ErrorCode::Ok);
        }
        if (const Entry* entry = find(number); entry != nullptr) {
            return succeed(entry->code);
        }
        return fail();
    }

    // 枚举名。
    if (const Entry* entry = find(text); entry != nullptr) {
        return succeed(entry->code);
    }
    return fail();
}

int exit_code(ErrorCode code) {
    if (code == ErrorCode::Ok) {
        return 0;
    }
    const Entry* entry = find(code);
    return entry != nullptr ? entry->exit_code : 1;
}

std::string_view default_message(ErrorCode code) {
    if (code == ErrorCode::Ok) {
        return "成功";
    }
    const Entry* entry = find(code);
    return entry != nullptr ? entry->message : std::string_view{"未知错误"};
}

Error make_error(ErrorCode code) {
    return Error{code, std::string(default_message(code))};
}

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

}  // namespace fmt
