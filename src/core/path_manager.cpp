#include "fmt/core/path_manager.hpp"

#include <utility>

#include "fmt/core/path.hpp"

namespace fmt {
namespace {

// 目录片段必须是单个名字：不能为空、不能带路径分隔符、不能是 . 或 ..
Status check_segment(std::string_view value, std::string_view label) {
    if (value.empty()) {
        return make_error(ErrorCode::InvalidArgument, std::string(label) + "不能为空");
    }
    if (value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos) {
        return make_error(ErrorCode::FileNameSeparator,
                          std::string(label) + "不能包含路径分隔符");
    }
    if (value == "." || value == "..") {
        return make_error(ErrorCode::PathEscape, std::string(label) + "不能是 . 或 ..");
    }
    return std::monostate{};
}

bool all_digits(std::string_view value, std::size_t length) {
    if (value.size() != length) {
        return false;
    }
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

}  // namespace

PathManager::PathManager(std::filesystem::path root) : root_(std::move(root)) {}

const std::vector<std::string>& PathManager::required_directories() {
    static const std::vector<std::string> kDirectories{"repository", "trash", "config", "data",
                                                       "log", "temp"};
    return kDirectories;
}

Result<std::filesystem::path> PathManager::build(std::filesystem::path base, std::string_view user,
                                                 std::string_view bucket, const DateParts& date,
                                                 std::string_view file_name) const {
    for (const auto& [value, label] :
         {std::pair<std::string_view, std::string_view>{user, "用户"},
          std::pair<std::string_view, std::string_view>{bucket, "Bucket"},
          std::pair<std::string_view, std::string_view>{file_name, "文件名"}}) {
        if (const Status status = check_segment(value, label); !ok(status)) {
            return *error_of(status);
        }
    }

    if (!all_digits(date.year, 4) || !all_digits(date.month, 2) || !all_digits(date.day, 2)) {
        return make_error(ErrorCode::InvalidArgument, "日期片段必须是 YYYY / MM / DD");
    }

    std::filesystem::path path = std::move(base);
    path /= path_from_utf8(std::string(user));
    path /= path_from_utf8(std::string(bucket));
    path /= date.year;
    path /= date.month;
    path /= date.day;
    path /= path_from_utf8(std::string(file_name));
    return path;
}

Result<std::filesystem::path> PathManager::repository_file(std::string_view user,
                                                           std::string_view bucket,
                                                           const DateParts& date,
                                                           std::string_view file_name) const {
    return build(repository(), user, bucket, date, file_name);
}

Result<std::filesystem::path> PathManager::trash_file(std::string_view user,
                                                      std::string_view bucket,
                                                      const DateParts& date,
                                                      std::string_view file_name) const {
    return build(trash(), user, bucket, date, file_name);
}

}  // namespace fmt
