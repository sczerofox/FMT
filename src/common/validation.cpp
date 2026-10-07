#include "fmt/common/validation.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace fmt {
namespace {

// Windows 文件名里不允许出现的字符（路径分隔符单独判断，好给出更准的错误码）。
constexpr std::string_view kInvalidChars = "<>:\"|?*";

bool is_ascii_alpha(char ch) {
    const auto value = static_cast<unsigned char>(ch);
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

// 控制字符与 DEL；UTF-8 多字节序列的字节都 >= 0x80，不受影响。
bool has_control(std::string_view name) {
    for (const char ch : name) {
        const auto value = static_cast<unsigned char>(ch);
        if (value < 0x20 || value == 0x7F) {
            return true;
        }
    }
    return false;
}

bool has_invalid_char(std::string_view name) {
    for (const char ch : name) {
        if (kInvalidChars.find(ch) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool has_separator(std::string_view name) {
    return name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos;
}

std::string to_lower_ascii(std::string_view text) {
    std::string result(text);
    for (char& ch : result) {
        if (is_ascii_alpha(ch)) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    return result;
}

std::vector<std::string> reserved_names() {
    std::vector<std::string> names{"con", "prn", "aux", "nul"};
    for (int i = 1; i <= 9; ++i) {
        names.push_back("com" + std::to_string(i));
        names.push_back("lpt" + std::to_string(i));
    }
    return names;
}

// 名称以 '.' 或 ' ' 结尾在 Windows 上会被静默截断，直接拒绝，别让用户以为存住了。
bool has_bad_tail(std::string_view name) {
    return !name.empty() && (name.back() == '.' || name.back() == ' ');
}

}  // namespace

bool is_windows_reserved_name(std::string_view name) {
    const std::size_t dot = name.find('.');
    const std::string stem = to_lower_ascii(dot == std::string_view::npos ? name : name.substr(0, dot));

    static const std::vector<std::string> kReserved = reserved_names();
    for (const std::string& reserved : kReserved) {
        if (stem == reserved) {
            return true;
        }
    }
    return false;
}

Status validate_bucket_name(std::string_view name) {
    if (name.empty()) {
        return make_error(ErrorCode::BucketNameInvalid, "Bucket 名称不能为空");
    }
    if (name.size() > kMaxNameBytes) {
        return make_error(ErrorCode::BucketNameInvalid,
                          "Bucket 名称过长（上限 " + std::to_string(kMaxNameBytes) + " 字节）");
    }
    if (has_separator(name)) {
        return make_error(ErrorCode::BucketNameInvalid, "Bucket 名称不能包含路径分隔符");
    }
    if (name == "." || name == "..") {
        return make_error(ErrorCode::BucketNameInvalid, "Bucket 名称不能是 . 或 ..");
    }
    if (has_control(name)) {
        return make_error(ErrorCode::BucketNameInvalid, "Bucket 名称不能包含控制字符");
    }
    if (has_invalid_char(name)) {
        return make_error(ErrorCode::BucketNameInvalid,
                          "Bucket 名称不能包含 < > : \" | ? * 这些字符");
    }
    if (is_windows_reserved_name(name)) {
        return make_error(ErrorCode::BucketNameInvalid,
                          "Bucket 名称不能是 Windows 保留设备名：" + std::string(name));
    }
    if (has_bad_tail(name)) {
        return make_error(ErrorCode::BucketNameInvalid, "Bucket 名称不能以点或空格结尾");
    }
    return std::monostate{};
}

Status validate_file_name(std::string_view name) {
    if (name.empty()) {
        return make_error(ErrorCode::FileNameEmpty, "文件名不能为空");
    }
    if (name.size() > kMaxNameBytes) {
        return make_error(ErrorCode::FileNameTooLong,
                          "文件名过长（上限 " + std::to_string(kMaxNameBytes) + " 字节）");
    }
    if (has_separator(name) || name == "." || name == "..") {
        return make_error(ErrorCode::FileNameSeparator, "文件名不能包含路径分隔符");
    }
    if (has_control(name) || has_invalid_char(name)) {
        return make_error(ErrorCode::FileNameInvalidChar,
                          "文件名不能包含 Windows 非法字符");
    }
    if (is_windows_reserved_name(name)) {
        return make_error(ErrorCode::FileNameReserved,
                          "文件名不能是 Windows 保留设备名：" + std::string(name));
    }
    if (has_bad_tail(name)) {
        return make_error(ErrorCode::FileNameInvalidChar, "文件名不能以点或空格结尾");
    }
    return std::monostate{};
}

}  // namespace fmt
