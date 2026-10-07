#include "fmt/service/service.hpp"

#include <utility>
#include <vector>

#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt::service {
namespace {

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
ServiceCallbacks g_callbacks;
HANDLE g_stop_event = nullptr;
DWORD g_checkpoint = 1;

// SetServiceStatus 上报。START_PENDING 期间必须定期推进 dwCheckPoint，
// 否则 SCM 会以 1053「服务没有及时响应启动或控制请求」判定启动失败。
void report(DWORD state, DWORD win32_exit_code, DWORD wait_hint_ms = 0,
            DWORD service_exit_code = 0) {
    if (g_status_handle == nullptr) {
        return;
    }
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    // 只接受 STOP 与 SHUTDOWN：本次明确不做 pause（SERVICE_ACCEPT_PAUSE_CONTINUE 不声明）。
    g_status.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    g_status.dwWin32ExitCode = win32_exit_code;
    g_status.dwServiceSpecificExitCode = service_exit_code;
    g_status.dwWaitHint = wait_hint_ms;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : g_checkpoint++;
    SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI handler_ex(DWORD control, DWORD, void*, void*) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            report(SERVICE_STOP_PENDING, NO_ERROR, 30000);
            if (g_stop_event != nullptr) {
                SetEvent(g_stop_event);
            }
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;  // 立即返回，SCM 要的就是当前状态
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, handler_ex, nullptr);
    if (g_status_handle == nullptr) {
        return;
    }

    report(SERVICE_START_PENDING, NO_ERROR, 3000);

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        report(SERVICE_STOPPED, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }

    Status initialized = std::monostate{};
    if (g_callbacks.initialize) {
        initialized = g_callbacks.initialize();
    }
    if (!ok(initialized)) {
        const Error& error = *error_of(initialized);
        report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0,
               static_cast<DWORD>(exit_code(error.code)));
        return;
    }

    report(SERVICE_RUNNING, NO_ERROR);

    WaitForSingleObject(g_stop_event, INFINITE);

    report(SERVICE_STOP_PENDING, NO_ERROR, 30000);
    if (g_callbacks.shutdown) {
        g_callbacks.shutdown();
    }
    report(SERVICE_STOPPED, NO_ERROR);
}

// 打开 SCM 并把访问被拒统一翻译成 FMT-603（CLI 会据此打印「需要管理员权限」）。
Result<SC_HANDLE> open_manager(DWORD access) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, access);
    if (manager == nullptr) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) {
            return make_error(ErrorCode::AdminRequired, "需要管理员权限");
        }
        return make_error(ErrorCode::ServiceOperationFailed,
                          "打开服务控制管理器失败（Win32 " + std::to_string(error) + "）");
    }
    return manager;
}

State map_state(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED:
            return State::Stopped;
        case SERVICE_START_PENDING:
            return State::StartPending;
        case SERVICE_STOP_PENDING:
            return State::StopPending;
        case SERVICE_RUNNING:
            return State::Running;
        case SERVICE_CONTINUE_PENDING:
            return State::ContinuePending;
        case SERVICE_PAUSE_PENDING:
            return State::PausePending;
        case SERVICE_PAUSED:
            return State::Paused;
        default:
            return State::Unknown;
    }
}

// 轮询等待服务进入目标状态（停止用）。超时返回 FMT-602。
Status wait_for_state(SC_HANDLE service, DWORD wanted, int timeout_ms) {
    const int step_ms = 200;
    for (int waited = 0; waited < timeout_ms; waited += step_ms) {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed)) {
            return make_error(ErrorCode::ServiceOperationFailed,
                              "查询服务状态失败（Win32 " + std::to_string(GetLastError()) + "）");
        }
        if (status.dwCurrentState == wanted) {
            return std::monostate{};
        }
        Sleep(static_cast<DWORD>(step_ms));
    }
    return make_error(ErrorCode::ServiceOperationFailed, "等待服务状态变化超时");
}

}  // namespace

std::string_view state_name(State state) {
    switch (state) {
        case State::NotInstalled:
            return "未安装";
        case State::Stopped:
            return "已停止";
        case State::StartPending:
            return "正在启动";
        case State::StopPending:
            return "正在停止";
        case State::Running:
            return "运行中";
        case State::ContinuePending:
            return "正在继续";
        case State::PausePending:
            return "正在暂停";
        case State::Paused:
            return "已暂停";
        case State::Unknown:
            return "未知";
    }
    return "未知";
}

std::filesystem::path state_directory() { return service_state_directory(); }

std::filesystem::path state_file() { return state_directory() / L"service.json"; }

Result<ServiceState> load_state_from(const std::filesystem::path& directory) {
    const std::filesystem::path path = directory / L"service.json";
    if (!file_exists(path)) {
        return ServiceState{};  // 首次运行：没有记录就是空状态
    }

    Result<nlohmann::json> parsed = read_json_file(path);
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    const nlohmann::json& value = std::get<nlohmann::json>(parsed);
    if (const Status version = check_version(value, ServiceState::kVersion); !ok(version)) {
        return *error_of(version);
    }

    ServiceState state;
    if (const auto iterator = value.find("current_root");
        iterator != value.end() && iterator->is_string()) {
        state.current_root = iterator->get<std::string>();
    }
    if (const auto iterator = value.find("host_path");
        iterator != value.end() && iterator->is_string()) {
        state.host_path = iterator->get<std::string>();
    }
    if (const auto iterator = value.find("installed_at");
        iterator != value.end() && iterator->is_string()) {
        state.installed_at = iterator->get<std::string>();
    }
    return state;
}

Status save_state_to(const std::filesystem::path& directory, const ServiceState& state) {
    if (const Status status = ensure_directory(directory); !ok(status)) {
        return status;
    }

    nlohmann::json value = nlohmann::json::object();
    value["version"] = ServiceState::kVersion;
    value["current_root"] = state.current_root;
    value["host_path"] = state.host_path;
    value["installed_at"] = state.installed_at;
    return write_json_file(directory / L"service.json", value);
}

Result<ServiceState> load_state() { return load_state_from(state_directory()); }

Status save_state(const ServiceState& state) { return save_state_to(state_directory(), state); }

State query_state() {
    Result<SC_HANDLE> manager = open_manager(SC_MANAGER_CONNECT);
    if (!ok(manager)) {
        return State::Unknown;
    }

    SC_HANDLE service = OpenServiceW(std::get<SC_HANDLE>(manager), kServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(std::get<SC_HANDLE>(manager));
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return State::NotInstalled;
        }
        return State::Unknown;
    }

    SERVICE_STATUS status{};
    const BOOL queried = QueryServiceStatus(service, &status);
    CloseServiceHandle(service);
    CloseServiceHandle(std::get<SC_HANDLE>(manager));
    if (!queried) {
        return State::Unknown;
    }
    return map_state(status.dwCurrentState);
}

Result<std::string> installed_binary_path() {
    Result<SC_HANDLE> manager = open_manager(SC_MANAGER_CONNECT);
    if (!ok(manager)) {
        return *error_of(manager);
    }

    SC_HANDLE service =
        OpenServiceW(std::get<SC_HANDLE>(manager), kServiceName, SERVICE_QUERY_CONFIG);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(std::get<SC_HANDLE>(manager));
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return make_error(ErrorCode::ServiceNotInstalled, "服务未安装");
        }
        return make_error(ErrorCode::ServiceOperationFailed,
                          "打开服务失败（Win32 " + std::to_string(error) + "）");
    }

    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    std::vector<char> buffer(needed);
    auto* config = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
    const BOOL queried = QueryServiceConfigW(service, config, needed, &needed);

    CloseServiceHandle(service);
    CloseServiceHandle(std::get<SC_HANDLE>(manager));

    if (!queried || config->lpBinaryPathName == nullptr) {
        return make_error(ErrorCode::ServiceOperationFailed, "查询服务配置失败");
    }

    // 注册时写的是 "路径"，取出来要去掉引号。
    std::string path = to_utf8(config->lpBinaryPathName);
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
        path = path.substr(1, path.size() - 2);
    }
    return path;
}

Status install(const std::string& binary_path, bool start_after_install) {
    if (binary_path.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少服务可执行文件路径");
    }

    Result<SC_HANDLE> manager = open_manager(SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!ok(manager)) {
        return *error_of(manager);
    }
    const SC_HANDLE manager_handle = std::get<SC_HANDLE>(manager);

    SC_HANDLE existing = OpenServiceW(manager_handle, kServiceName, SERVICE_QUERY_STATUS);
    if (existing != nullptr) {
        CloseServiceHandle(existing);
        CloseServiceHandle(manager_handle);
        return make_error(ErrorCode::ServiceAlreadyInstalled, "服务已安装");
    }

    const std::wstring command = L"\"" + to_wide(binary_path) + L"\"";
    SC_HANDLE service = CreateServiceW(
        manager_handle, kServiceName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
        nullptr /* LocalSystem */, nullptr);

    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager_handle);
        if (error == ERROR_SERVICE_EXISTS) {
            return make_error(ErrorCode::ServiceAlreadyInstalled, "服务已安装");
        }
        if (error == ERROR_ACCESS_DENIED) {
            return make_error(ErrorCode::AdminRequired, "需要管理员权限");
        }
        return make_error(ErrorCode::ServiceOperationFailed,
                          "创建服务失败（Win32 " + std::to_string(error) + "）");
    }

    // 异常自动恢复：不自建 watchdog。
    SC_ACTION actions[3] = {};
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = kRecoveryFirstDelayMs;
    actions[1].Type = SC_ACTION_RESTART;
    actions[1].Delay = kRecoverySecondDelayMs;
    actions[2].Type = SC_ACTION_RESTART;
    actions[2].Delay = kRecoveryLaterDelayMs;

    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = kRecoveryResetPeriodSeconds;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    Status result = std::monostate{};
    if (start_after_install) {
        if (!StartServiceW(service, 0, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_SERVICE_ALREADY_RUNNING) {
                result = make_error(ErrorCode::ServiceOperationFailed,
                                    "服务已安装，但启动失败（Win32 " + std::to_string(error) + "）");
            }
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager_handle);

    // 记录宿主路径与安装时间（服务自身状态，不属于业务数据）。
    if (Result<ServiceState> state = load_state(); ok(state)) {
        ServiceState updated = std::get<ServiceState>(state);
        updated.host_path = to_forward_slashes(binary_path);
        updated.installed_at = local_timestamp();
        (void)save_state(updated);
    }

    return result;
}

Status uninstall() {
    Result<SC_HANDLE> manager = open_manager(SC_MANAGER_CONNECT);
    if (!ok(manager)) {
        return *error_of(manager);
    }
    const SC_HANDLE manager_handle = std::get<SC_HANDLE>(manager);

    SC_HANDLE service = OpenServiceW(
        manager_handle, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager_handle);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return make_error(ErrorCode::ServiceNotInstalled, "服务未安装");
        }
        return make_error(ErrorCode::ServiceOperationFailed,
                          "打开服务失败（Win32 " + std::to_string(error) + "）");
    }

    // 先停再删：服务正在运行时 DeleteService 只是标记删除。
    SERVICE_STATUS status{};
    if (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED) {
        if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
            const DWORD error = GetLastError();
            if (error != ERROR_SERVICE_NOT_ACTIVE) {
                CloseServiceHandle(service);
                CloseServiceHandle(manager_handle);
                return make_error(ErrorCode::ServiceOperationFailed,
                                  "停止服务失败（Win32 " + std::to_string(error) + "）");
            }
        } else if (const Status waited = wait_for_state(service, SERVICE_STOPPED, 30000);
                   !ok(waited)) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager_handle);
            return *error_of(waited);
        }
    }

    const BOOL deleted = DeleteService(service);
    const DWORD error = GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager_handle);

    if (!deleted) {
        if (error == ERROR_SERVICE_MARKED_FOR_DELETE) {
            return std::monostate{};  // 已经标记删除，视为成功
        }
        return make_error(ErrorCode::ServiceOperationFailed,
                          "删除服务失败（Win32 " + std::to_string(error) + "）");
    }
    // 数据目录一个都不删：repository / trash / config / data / log 全部保留。
    return std::monostate{};
}

Status start() {
    Result<SC_HANDLE> manager = open_manager(SC_MANAGER_CONNECT);
    if (!ok(manager)) {
        return *error_of(manager);
    }
    const SC_HANDLE manager_handle = std::get<SC_HANDLE>(manager);

    SC_HANDLE service = OpenServiceW(manager_handle, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager_handle);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return make_error(ErrorCode::ServiceNotInstalled, "服务未安装");
        }
        return make_error(ErrorCode::ServiceOperationFailed,
                          "打开服务失败（Win32 " + std::to_string(error) + "）");
    }

    Status result = std::monostate{};
    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) {
            result = make_error(ErrorCode::ServiceOperationFailed,
                                "启动服务失败（Win32 " + std::to_string(error) + "）");
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager_handle);
    return result;
}

Status stop() {
    Result<SC_HANDLE> manager = open_manager(SC_MANAGER_CONNECT);
    if (!ok(manager)) {
        return *error_of(manager);
    }
    const SC_HANDLE manager_handle = std::get<SC_HANDLE>(manager);

    SC_HANDLE service = OpenServiceW(manager_handle, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager_handle);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return make_error(ErrorCode::ServiceNotInstalled, "服务未安装");
        }
        return make_error(ErrorCode::ServiceOperationFailed,
                          "打开服务失败（Win32 " + std::to_string(error) + "）");
    }

    SERVICE_STATUS status{};
    Status result = std::monostate{};
    if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        result = wait_for_state(service, SERVICE_STOPPED, 30000);
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) {
            result = make_error(ErrorCode::ServiceOperationFailed,
                                "停止服务失败（Win32 " + std::to_string(error) + "）");
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager_handle);
    return result;
}

DispatcherResult dispatch_service(const ServiceCallbacks& callbacks) {
    g_callbacks = callbacks;

    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), service_main},
        {nullptr, nullptr},
    };

    if (StartServiceCtrlDispatcherW(table)) {
        return DispatcherResult::BecameService;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        return DispatcherResult::NotService;  // 用户双击 / 命令行启动 → 走 CLI
    }
    return DispatcherResult::Failed;
}

}  // namespace fmt::service
