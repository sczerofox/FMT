// Windows Service：SCM 操作、服务状态文件、服务形态入口
//
// 四条命令：install / uninstall / start / stop（没有 pause，也没有 delete）。
// 四条命令都需要管理员权限，由 CLI 用 runas 提权后在这条进程里执行。
//
// 服务自身状态写在 %ProgramData%\FMT\service.json，**不属于业务数据**：
// 数据根可以随 fmt.exe 到处走，服务自身的记录只有一个。
#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "fmt/common/error.hpp"

namespace fmt::service {

inline constexpr wchar_t kServiceName[] = L"FMT";
inline constexpr wchar_t kDisplayName[] = L"FMT File Management Service";

// Recovery：第一次失败 5 秒、第二次 10 秒、后续 30 秒重启，失败计数 1 天重置。
inline constexpr DWORD kRecoveryFirstDelayMs = 5000;
inline constexpr DWORD kRecoverySecondDelayMs = 10000;
inline constexpr DWORD kRecoveryLaterDelayMs = 30000;
inline constexpr DWORD kRecoveryResetPeriodSeconds = 24 * 60 * 60;

enum class State {
    NotInstalled,
    Stopped,
    StartPending,
    StopPending,
    Running,
    ContinuePending,
    PausePending,
    Paused,
    Unknown,
};

std::string_view state_name(State state);

// 一次状态查询的完整结果。等待类状态会带上 SCM 自己估计的 dwWaitHint：
// 「还要多久」由 SCM 说，别写死秒数。
struct StatusInfo {
    State state = State::Unknown;
    DWORD wait_hint_ms = 0;
    DWORD win32_exit_code = 0;
    DWORD service_exit_code = 0;
};

// 查询服务状态。**不需要管理员权限**。
// 「未安装」是一个正常状态（state = NotInstalled）；只有查询本身失败才返回错误。
Result<StatusInfo> query_status();

// ---- 服务自身状态 ----
std::filesystem::path state_directory();  // %ProgramData%\FMT
std::filesystem::path state_file();       // %ProgramData%\FMT\service.json

struct ServiceState {
    static constexpr int kVersion = 1;

    int version = kVersion;
    std::string current_root;  // 当前数据根
    std::string host_path;     // 服务宿主 exe 的绝对路径
    std::string installed_at;  // 安装时间
};

Result<ServiceState> load_state_from(const std::filesystem::path& directory);
Status save_state_to(const std::filesystem::path& directory, const ServiceState& state);
Result<ServiceState> load_state();
Status save_state(const ServiceState& state);

// ---- 查询：不需要管理员权限 ----
State query_state();

// 已安装服务注册的可执行文件路径（去掉引号）；未安装返回空字符串。
Result<std::string> installed_binary_path();

// 服务最近一次启动失败的原因。
// ServiceMain 初始化失败时会把 **FMT 编号** 写进 dwServiceSpecificExitCode
// （不是退出码），CLI 据此能说出「FMT-008 配置错误」，而不是盲目重装服务。
// 没有失败信息时返回错误。
Result<ErrorCode> last_start_failure();

// ---- 四条命令：需要管理员权限 ----
// install：创建服务（自动启动、LocalSystem）-> 配 Recovery -> 可选立即启动。
//          已存在返回 FMT-600。
Status install(const std::string& binary_path, bool start_after_install);
// uninstall：先停止再删除；**不删** repository / trash / config / data / log。
Status uninstall();
Status start();
Status stop();

// ---- 服务形态 ----
struct ServiceCallbacks {
    // 服务启动时的初始化；返回错误则服务以该错误码停止。
    std::function<Status()> initialize;
    // 收到停止/关机控制后的收尾（等在途操作、停 HTTP）。
    std::function<void()> shutdown;
    // 服务控制事件（STOP / SHUTDOWN），用于写日志。
    // 由 SCM 的控制线程调用：实现里不要做耗时工作。
    std::function<void(const char* event)> on_control;
};

enum class DispatcherResult {
    BecameService,  // 被 SCM 启动，已经作为服务运行并退出
    NotService,     // 不是 SCM 启动（用户双击/命令行）→ 调用方走 CLI
    Failed,         // 其它错误（FMT-602 / 退出码 8）
};

// 交给 SCM 的入口。必须在 wmain 开头尽早调用：SCM 只等 30 秒。
DispatcherResult dispatch_service(const ServiceCallbacks& callbacks);

}  // namespace fmt::service
