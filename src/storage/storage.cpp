#include "fmt/storage/storage.hpp"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <system_error>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"

namespace fmt {
namespace {

std::filesystem::path temporary_path_for(const std::filesystem::path& path) {
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    return temporary;
}

}  // namespace

Result<std::string> read_text_file(const std::filesystem::path& path) {
    if (!file_exists(path)) {
        return make_error(ErrorCode::FileNotFound, "文件不存在：" + path_to_utf8(path));
    }

    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) {
        return make_error(ErrorCode::IoError, "无法打开文件：" + path_to_utf8(path));
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (stream.bad()) {
        return make_error(ErrorCode::IoError, "读取失败：" + path_to_utf8(path));
    }
    return buffer.str();
}

Status write_text_file_atomic(const std::filesystem::path& path, std::string_view content) {
    const std::filesystem::path temporary = temporary_path_for(path);

    {
        std::ofstream stream(temporary, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!stream) {
            return make_error(ErrorCode::IoError, "无法写入临时文件：" + path_to_utf8(temporary));
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.flush();
        if (!stream) {
            return make_error(ErrorCode::IoError, "写入临时文件失败：" + path_to_utf8(temporary));
        }
    }

    // 替换前先读回校验：内容不全就不动正式文件。
    Result<std::string> written = read_text_file(temporary);
    if (!ok(written)) {
        return *error_of(written);
    }
    if (std::get<std::string>(written).size() != content.size()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return make_error(ErrorCode::IoError, "临时文件校验失败：" + path_to_utf8(temporary));
    }

    // MoveFileExW 带 MOVEFILE_REPLACE_EXISTING 是原子替换；
    // std::filesystem::rename 在目标已存在时会失败。
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD code = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return make_error(ErrorCode::IoError,
                          "替换文件失败：" + path_to_utf8(path) + "（Win32 " +
                              std::to_string(code) + "）");
    }

    return std::monostate{};
}

Result<nlohmann::json> read_json_file(const std::filesystem::path& path) {
    Result<std::string> text = read_text_file(path);
    if (!ok(text)) {
        return *error_of(text);
    }

    try {
        return nlohmann::json::parse(std::get<std::string>(text));
    } catch (const nlohmann::json::exception& error) {
        return make_error(ErrorCode::JsonParseError,
                          "JSON 格式错误：" + path_to_utf8(path) + "（" + error.what() + "）");
    }
}

Status write_json_file(const std::filesystem::path& path, const nlohmann::json& value) {
    std::string text = value.dump(2);
    text += '\n';
    return write_text_file_atomic(path, text);
}

nlohmann::json make_collection(int version, std::string_view collection) {
    nlohmann::json value = nlohmann::json::object();
    value["version"] = version;
    value[std::string(collection)] = nlohmann::json::array();
    return value;
}

Status check_version(const nlohmann::json& value, int supported) {
    if (!value.is_object()) {
        return make_error(ErrorCode::JsonParseError, "JSON 顶层必须是对象");
    }
    if (!value.contains("version")) {
        return make_error(ErrorCode::JsonParseError, "JSON 缺少 version 字段");
    }
    if (!value["version"].is_number_integer()) {
        return make_error(ErrorCode::JsonParseError, "version 字段必须是整数");
    }

    const int version = value["version"].get<int>();
    if (version != supported) {
        return make_error(ErrorCode::JsonUnsupportedVersion,
                          "JSON 版本 " + std::to_string(version) + " 不受支持，本程序只支持 " +
                              std::to_string(supported));
    }
    return std::monostate{};
}

Status ensure_directory(const std::filesystem::path& path) {
    if (directory_exists(path)) {
        return std::monostate{};
    }

    std::error_code code;
    std::filesystem::create_directories(path, code);
    if (code && !directory_exists(path)) {
        return make_error(ErrorCode::DirectoryCreateFailed,
                          "无法创建目录：" + path_to_utf8(path) + "（" + code.message() + "）");
    }
    return std::monostate{};
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code code;
    return std::filesystem::exists(path, code);
}

bool directory_exists(const std::filesystem::path& path) {
    std::error_code code;
    return std::filesystem::is_directory(path, code);
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code code;
    return std::filesystem::is_regular_file(path, code);
}

}  // namespace fmt
