#include "fmt/cli/cli.hpp"

#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <utility>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/service/service.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt::cli {
namespace {

constexpr DWORD kElevationTimeoutMs = 60000;

std::filesystem::path temp_directory() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) {
        return std::filesystem::temp_directory_path();
    }
    return std::filesystem::path(std::wstring(buffer, length));
}

// 提权副本把结果写进这个文件；父进程等它退出后读。
std::string quote_for_command_line(const std::string& value) {
    return "\"" + value + "\"";
}

}  // namespace

std::string result_file_for(unsigned long process_id) {
    return path_to_utf8(temp_directory() /
                        (L"fmt-elev-" + std::to_wstring(process_id) + L".json"));
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL queried =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    CloseHandle(token);

    return queried && elevation.TokenIsElevated != 0;
}

Result<ElevatedOutcome> elevate_service_command(const std::string& operation,
                                                const std::string& self_path) {
    if (self_path.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少自身可执行文件路径");
    }

    const std::string result_path = result_file_for(GetCurrentProcessId());
    {
        std::error_code code;
        std::filesystem::remove(path_from_utf8(result_path), code);
    }
    log_info("Cli", "启动提权副本：--elevated " + operation + "（结果文件 " + result_path + "）");

    const std::wstring parameters = L"--elevated " + to_wide(operation) + L" --result " +
                                    to_wide(result_path);
    // 必须用具名变量：ShellExecuteExW 只保存指针，临时对象在这里析构的话
    // lpFile 就成了悬垂指针（表现为 Win32 1155 ERROR_NO_ASSOCIATION）。
    const std::filesystem::path self = path_from_utf8(self_path);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = self.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;  // 提权副本不留窗口：runas 会另开控制台，必须藏起来

    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            log_warn("Cli", "用户在 UAC 里点了「否」，取消提权");
            return make_error(ErrorCode::PermissionDenied, "用户取消提权");
        }
        if (error == ERROR_ACCESS_DENIED) {
            log_error("Cli", "提权被拒绝（Win32 5）");
            return make_error(ErrorCode::AdminRequired, "提权被拒绝");
        }
        log_error("Cli", "无法启动提权副本（Win32 " + std::to_string(error) + "）");
        return make_error(ErrorCode::ServiceOperationFailed,
                          "无法启动提权副本（Win32 " + std::to_string(error) + "）");
    }

    const DWORD wait = WaitForSingleObject(info.hProcess, kElevationTimeoutMs);
    DWORD child_exit = 0;
    GetExitCodeProcess(info.hProcess, &child_exit);
    CloseHandle(info.hProcess);

    if (wait == WAIT_TIMEOUT) {
        return make_error(ErrorCode::ServiceOperationFailed, "提权操作超时");
    }

    // 结果文件是提权副本唯一的回传通道。
    const std::filesystem::path result_file = path_from_utf8(result_path);
    if (!file_exists(result_file)) {
        return make_error(ErrorCode::ServiceOperationFailed,
                          "提权副本没有返回结果（退出码 " + std::to_string(child_exit) + "）");
    }

    Result<nlohmann::json> parsed = read_json_file(result_file);
    {
        std::error_code code;
        std::filesystem::remove(result_file, code);
    }
    if (!ok(parsed)) {
        return *error_of(parsed);
    }

    const nlohmann::json& value = std::get<nlohmann::json>(parsed);
    ElevatedOutcome outcome;
    if (const auto iterator = value.find("ok"); iterator != value.end() && iterator->is_boolean()) {
        outcome.ok = iterator->get<bool>();
    }
    if (const auto iterator = value.find("code"); iterator != value.end() && iterator->is_string()) {
        bool known = false;
        outcome.code = code_from_string(iterator->get<std::string>(), &known);
        if (!known) {
            outcome.code = ErrorCode::InvalidArgument;
        }
    }
    if (const auto iterator = value.find("message");
        iterator != value.end() && iterator->is_string()) {
        outcome.message = iterator->get<std::string>();
    }
    if (const auto iterator = value.find("exit"); iterator != value.end() && iterator->is_number()) {
        outcome.exit_code = iterator->get<int>();
    }
    if (outcome.message.empty()) {
        outcome.message = std::string(default_message(outcome.code));
    }
    return outcome;
}

int run_elevated(const std::vector<std::string>& args) {
    // 提权副本是独立进程：它也要把自己做了什么写进同一个 fmt.log。
    std::unique_ptr<Logger> file_logger;
    if (Result<std::unique_ptr<Logger>> opened =
            open_cli_logger(path_to_utf8(executable_directory()));
        ok(opened)) {
        file_logger = std::move(std::get<std::unique_ptr<Logger>>(opened));
        set_logger(file_logger.get());
    }

    std::string operation;
    std::string result_path;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--elevated" && i + 1 < args.size()) {
            operation = args[++i];
        } else if (args[i] == "--result" && i + 1 < args.size()) {
            result_path = args[++i];
        }
    }

    if (operation.empty()) {
        log_error("Elevated", "提权副本缺少 --elevated 参数");
        return exit_code(ErrorCode::InvalidArgument);
    }

    log_info("Elevated", "提权副本开始执行：" + operation);

    Status status = std::monostate{};
    if (operation == "install") {
        status = service::install(path_to_utf8(executable_path()), true);
    } else if (operation == "uninstall") {
        status = service::uninstall();
    } else if (operation == "start") {
        status = service::start();
    } else if (operation == "stop") {
        status = service::stop();
    } else if (operation == "reinstall") {
        // 宿主 exe 丢了：一次 UAC 里做完卸载 + 重新安装指向当前目录。
        const Status removed = service::uninstall();
        const bool removable =
            ok(removed) || error_of(removed)->code == ErrorCode::ServiceNotInstalled;
        status = removable ? service::install(path_to_utf8(executable_path()), true) : removed;
    } else {
        status = make_error(ErrorCode::InvalidArgument, "未知的提权操作：" + operation);
    }

    const bool succeeded = ok(status);
    const ErrorCode code = succeeded ? ErrorCode::Ok : error_of(status)->code;
    const std::string message =
        succeeded ? std::string(default_message(ErrorCode::Ok)) : error_of(status)->message;
    const int exit = exit_code(code);

    if (succeeded) {
        log_info("Elevated", "提权副本执行成功：" + operation + "（错误码 0）");
    } else {
        log_error("Elevated", "提权副本执行失败：" + operation + " -> " + code_string(code) + " " +
                                  message + "（错误码 " + std::to_string(exit) + "）");
    }

    if (!result_path.empty()) {
        nlohmann::json value = nlohmann::json::object();
        value["ok"] = succeeded;
        value["code"] = code_string(code);
        value["message"] = message;
        value["exit"] = exit;
        const Status written = write_json_file(path_from_utf8(result_path), value);
        if (!ok(written)) {
            log_error("Elevated", "结果文件写入失败：" + error_of(written)->message);
            return exit_code(error_of(written)->code);
        }
    }

    set_logger(nullptr);
    return exit;
}

std::vector<std::string> split_command(const std::string& line) {
    std::vector<std::string> parts;
    std::string current;
    bool in_quotes = false;

    std::string text = trim(line);
    // 管道或重定向进来的输入常常带一个 UTF-8 BOM，先摘掉。
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    for (const char ch : text) {
        if (ch == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && (ch == ' ' || ch == '\t')) {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

}  // namespace fmt::cli
