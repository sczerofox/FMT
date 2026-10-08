#include "fmt/storage/storage.hpp"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"

namespace fmt {
namespace {

// 原子写的临时文件名必须**每个进程、每次调用都不同**。
//
// 原来固定用 `<目标>.tmp`，于是同一个目标被两个进程同时写时互相踩：
// 典型场景是 `service install`——安装器在启动服务之后写 `service.json`
//（记录 host_path / installed_at），而服务启动时也写同一个文件。两者共用
// `service.json.tmp`：先完成的一方把它 rename 成正式文件，另一方接着做
// **读回校验**时文件已经不在了，报「无法打开文件 …service.json.tmp」。
// 更糟的是安装器那边 `(void)save_state(...)` 忽略了返回值——两边都可能失败，
// 于是 service.json 静默地一直不更新（installed_at 停在旧时间）。
//
// 名字里带 pid 与序号之后，两个写者各写各的临时文件，rename 是「后完成者胜」，
// 正常的结果就是最后那份内容，而不是某一边的报错。
std::filesystem::path temporary_path_for(const std::filesystem::path& path) {
    static std::atomic<unsigned> counter{0};
    std::filesystem::path temporary = path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                 std::to_wstring(counter.fetch_add(1)) + L".tmp";
    return temporary;
}

// 同一个进程内的写入串行化。跨进程的竞争（例如安装器与服务启动同时写
// service.json）靠下面的替换重试兜底。
std::mutex g_write_mutex;

// MoveFileExW 的原子替换在**目标正在被另一个写者替换/打开**时会短暂失败
// （ERROR_ACCESS_DENIED / ERROR_SHARING_VIOLATION）。这类失败等几毫秒再来就行，
// 不该把它当成永久错误报给用户。
bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to,
                  DWORD* last_error) {
    constexpr int kAttempts = 40;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            return true;
        }
        const DWORD code = GetLastError();
        if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION &&
            code != ERROR_LOCK_VIOLATION && code != ERROR_FILE_NOT_FOUND) {
            *last_error = code;
            return false;
        }
        *last_error = code;
        Sleep(5);
    }
    return false;
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
    // 进程内串行：同一个目标被多个线程写时，不必让它们去竞争替换那一步
    const std::lock_guard<std::mutex> guard(g_write_mutex);

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
    DWORD code = 0;
    if (!replace_file(temporary, path, &code)) {
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
