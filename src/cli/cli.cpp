#include "fmt/cli/cli.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/core/path_manager.hpp"
#include "fmt/ipc/pipe.hpp"
#include "fmt/service/service.hpp"
#include "fmt/version.hpp"

namespace fmt::cli {
namespace {
Logger* g_logger = nullptr;
}  // namespace

void set_logger(Logger* logger) { g_logger = logger; }

Logger* logger() { return g_logger; }

void log(LogLevel level, std::string_view module, const std::string& message) {
    if (g_logger != nullptr) {
        g_logger->log(level, module, message);
    }
}

void log_info(std::string_view module, const std::string& message) {
    log(LogLevel::Info, module, message);
}

void log_warn(std::string_view module, const std::string& message) {
    log(LogLevel::Warn, module, message);
}

void log_error(std::string_view module, const std::string& message) {
    log(LogLevel::Error, module, message);
}

Result<std::unique_ptr<Logger>> open_cli_logger(const std::string& data_root) {
    Logger::Options options;
    // 控制台由 CLI 自己打印，日志器不要再打印一遍。
    options.info_to_console = false;
    options.warn_error_to_console = false;

    const PathManager paths{path_from_utf8(data_root)};
    // log/ 不是业务数据：CLI 只允许创建这一个目录。
    return Logger::open(paths.log(), options);
}

namespace {

constexpr wchar_t kSingletonMutex[] = L"Local\\FMT.CLI.v1";
constexpr wchar_t kConsoleTitle[] = L"FMT";

// 服务刚被 service start 拉起来时监听还没就绪，连接要给它一点时间。
constexpr int kConnectWaitMs = 5000;

// 服务当前数据根与本进程不同时，点明服务侧日志写在哪里：
// 两个进程各写自己数据根下的 log/fmt.log（根一样时才是同一个文件）。
void note_service_root(const Options& options) {
    Result<service::ServiceState> state = service::load_state();
    if (!ok(state)) {
        return;
    }
    const std::string service_root = std::get<service::ServiceState>(state).current_root;
    if (service_root.empty() || iequals(service_root, to_forward_slashes(options.data_root))) {
        return;
    }
    log_warn("Cli", "服务当前数据根是 " + service_root + "，服务侧日志写在该根的 log/fmt.log；" +
                        "本进程日志写在本目录的 log/fmt.log");
}

void print_version() {
    std::printf("%.*s\n", static_cast<int>(version::STRING.size()), version::STRING.data());
}

void print_usage() {
    std::printf(
        "FMT %.*s - Windows 文件管理系统\n"
        "\n"
        "用法：\n"
        "  fmt.exe --help               显示本帮助\n"
        "  fmt.exe --version            显示版本\n"
        "  fmt.exe service install      安装并启动服务（需要管理员权限）\n"
        "  fmt.exe service uninstall    停止并删除服务（需要管理员权限）\n"
        "  fmt.exe service start        启动服务（需要管理员权限）\n"
        "  fmt.exe service stop         停止服务（需要管理员权限）\n"
        "\n"
        "直接双击进入交互式命令行：\n"
        "  fmt> service stop\n"
        "  fmt> file list\n"
        "  fmt> exit\n"
        "\n"
        "退出码：0 成功  1 通用错误  2 参数错误  3 对象不存在  4 冲突\n"
        "        5 权限/访问  6 数据一致性  7 配置错误  8 Service 错误\n",
        static_cast<int>(version::STRING.size()), version::STRING.data());
}

std::string service_state_line(service::State state) {
    return state == service::State::Running ? "Service Running..." : "Service Stopped...";
}

// 刚安装或刚启动时服务还在 START_PENDING：等它落定再显示状态，
// 否则横幅会在服务已经起来的情况下写 Service Stopped...
service::State settle_state(service::State state, int timeout_ms) {
    constexpr int kStepMs = 100;
    for (int waited = 0; waited < timeout_ms; waited += kStepMs) {
        if (state != service::State::StartPending && state != service::State::StopPending) {
            break;
        }
        Sleep(kStepMs);
        state = service::query_state();
    }
    return state;
}

// ---- 单实例：已有窗口就把它拉到前面，不再开第二个 ----
struct ActivateContext {
    DWORD self_pid = 0;
    bool activated = false;
};

BOOL CALLBACK activate_callback(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<ActivateContext*>(parameter);

    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == context->self_pid) {
        return TRUE;  // 跳过自己
    }
    if (GetWindow(window, GW_OWNER) != nullptr || !IsWindowVisible(window)) {
        return TRUE;
    }

    wchar_t title[256] = {};
    GetWindowTextW(window, title, 256);
    if (std::wstring_view(title).find(kConsoleTitle) == std::wstring_view::npos) {
        return TRUE;
    }

    ShowWindow(window, SW_RESTORE);
    if (!SetForegroundWindow(window)) {
        FlashWindow(window, TRUE);  // 前台锁定时的降级
    }
    context->activated = true;
    return FALSE;
}

bool activate_existing_window() {
    ActivateContext context;
    context.self_pid = GetCurrentProcessId();
    EnumWindows(activate_callback, reinterpret_cast<LPARAM>(&context));
    return context.activated;
}

// 用户可以直接键入的 service 命令；reinstall 是引导流程内部使用的，
// 不在命令集里（文档冻结的是四条命令）。
bool is_user_service_command(const std::string& operation) {
    return operation == "install" || operation == "uninstall" || operation == "start" ||
           operation == "stop";
}

// ---- service 四条命令：每条都提权 ----
int run_service_command(const std::string& operation, const Options& options) {
    log_info("Cli", "service " + operation + "：需要管理员权限，开始提权");
    std::printf("需要管理员权限\n");
    std::printf("正在提权...\n");
    std::fflush(stdout);

    const Result<ElevatedOutcome> elevated =
        elevate_service_command(operation, options.self_path);
    if (!ok(elevated)) {
        const Error& error = *error_of(elevated);
        log_error("Cli", "service " + operation + " 提权失败：" + code_string(error.code) + " " +
                             error.message);
        std::fprintf(stderr, "执行失败：%s %s\n", code_string(error.code).c_str(),
                     error.message.c_str());
        std::fprintf(stderr, "错误码：%d\n", exit_code(error.code));
        return exit_code(error.code);
    }

    const ElevatedOutcome& outcome = std::get<ElevatedOutcome>(elevated);
    if (outcome.ok) {
        log_info("Cli", "service " + operation + " 执行成功（错误码 0）");
        std::printf("执行成功...\n");
        std::printf("错误码：0\n");
        return 0;
    }

    log_error("Cli", "service " + operation + " 执行失败：" + code_string(outcome.code) + " " +
                         outcome.message + "（错误码 " + std::to_string(outcome.exit_code) + "）");
    std::fprintf(stderr, "执行失败：%s %s\n", code_string(outcome.code).c_str(),
                 outcome.message.c_str());
    std::fprintf(stderr, "错误码：%d\n", outcome.exit_code);
    return outcome.exit_code;
}

// ---- 业务命令：走命名管道 ----
struct Session {
    ipc::PipeClient client;
    bool connected = false;
    int next_id = 1;
};

Status ensure_connected(Session& session, const Options& options) {
    if (session.connected) {
        return std::monostate{};
    }

    Result<ipc::PipeClient> connected = ipc::PipeClient::connect_waiting(kConnectWaitMs);
    if (!ok(connected)) {
        return *error_of(connected);
    }
    session.client = std::move(std::get<ipc::PipeClient>(connected));

    // 首帧 hello 声明数据根：根不一样时服务会切换并做幂等初始化。
    ipc::Request hello;
    hello.id = session.next_id++;
    hello.op = "hello";
    hello.root = options.data_root;
    hello.pid = GetCurrentProcessId();

    Result<ipc::Response> response = session.client.call(hello, ipc::kCommandTimeoutMs);
    if (!ok(response)) {
        return *error_of(response);
    }
    if (!std::get<ipc::Response>(response).ok) {
        return std::get<ipc::Response>(response).error;
    }

    session.connected = true;
    log_info("Ipc", "已连接服务，数据根声明为：" + to_forward_slashes(options.data_root));
    return std::monostate{};
}

void print_failure(const Error& error) {
    std::fprintf(stderr, "执行失败：%s %s\n", code_string(error.code).c_str(),
                 error.message.c_str());
    std::fprintf(stderr, "错误码：%d\n", exit_code(error.code));
}

int run_business_command(const std::vector<std::string>& parts, Session& session,
                         const Options& options) {
    const std::string operation = parts[0] + "." + parts[1];

    if (const Status status = ensure_connected(session, options); !ok(status)) {
        log_error("Cli", "命令 " + operation + " 无法连接服务：" + error_of(status)->message);
        print_failure(*error_of(status));
        return exit_code(error_of(status)->code);
    }

    ipc::Request request;
    request.id = session.next_id++;
    request.op = operation;
    request.root = options.data_root;
    request.pid = GetCurrentProcessId();

    nlohmann::json arguments = nlohmann::json::array();
    for (std::size_t i = 2; i < parts.size(); ++i) {
        arguments.push_back(parts[i]);
    }
    if (!arguments.empty()) {
        request.args["argv"] = arguments;
    }

    log_info("Cli", "命令 " + operation + " 已发送（id " + std::to_string(request.id) + "）");

    Result<ipc::Response> response = session.client.call(request, ipc::kCommandTimeoutMs);
    if (!ok(response)) {
        session.connected = false;
        log_error("Cli", "命令 " + operation + " 通信失败：" + error_of(response)->message);
        print_failure(*error_of(response));
        return exit_code(error_of(response)->code);
    }

    const ipc::Response& value = std::get<ipc::Response>(response);
    if (value.ok) {
        log_info("Cli", "命令 " + operation + " 执行成功（错误码 0）");
        if (!value.data.is_null() && !(value.data.is_object() && value.data.empty())) {
            std::printf("%s\n", value.data.dump(2).c_str());
        }
        std::printf("执行成功...\n");
        std::printf("错误码：0\n");
        return 0;
    }

    log_error("Cli", "命令 " + operation + " 执行失败：" + code_string(value.error.code) + " " +
                         value.error.message);
    print_failure(value.error);
    return exit_code(value.error.code);
}

// ---- 交互循环 ----
int run_interactive(const Options& options, service::State state) {
    std::printf("FMT %.*s\n", static_cast<int>(version::STRING.size()), version::STRING.data());
    std::printf("%s\n", service_state_line(state).c_str());
    log_info("Cli", "进入交互循环，数据根：" + to_forward_slashes(options.data_root));

    Session session;
    while (true) {
        std::printf("fmt> ");
        std::fflush(stdout);

        std::string line;
        if (!std::getline(std::cin, line)) {
            std::printf("\n");
            break;  // Ctrl+Z 或输入结束
        }

        const std::vector<std::string> parts = split_command(line);
        if (parts.empty()) {
            continue;
        }

        // 用户敲了什么就记什么：日志要能跟着 CLI 走。
        log_info("Cli", "fmt> " + trim(line));

        const std::string& head = parts[0];
        if (head == "exit" || head == "quit") {
            break;
        }
        if (head == "help" || head == "--help") {
            print_usage();
            continue;
        }
        if (head == "service") {
            if (parts.size() < 2 || !is_user_service_command(parts[1])) {
                std::fprintf(stderr, "用法：service install | uninstall | start | stop\n");
                continue;
            }
            run_service_command(parts[1], options);
            continue;
        }
        if (parts.size() < 2) {
            std::fprintf(stderr, "未知命令：%s（输入 help 查看用法）\n", head.c_str());
            continue;
        }

        run_business_command(parts, session, options);
    }
    log_info("Cli", "退出交互循环");
    return 0;
}

// ---- 双击引导：查 SCM -> 需要时提权 -> 进循环 ----
int bootstrap_and_run(const Options& options) {
    service::State state = service::query_state();
    log_info("Service", "当前状态：" + std::string(service::state_name(state)));

    if (state == service::State::NotInstalled) {
        log_info("Service", "服务未安装 -> 首次安装并启动");
        run_service_command("install", options);  // 首次双击：一次 UAC，装 + 启动
        state = service::query_state();
    } else if (state == service::State::Stopped) {
        log_info("Service", "服务已停止 -> 启动");
        run_service_command("start", options);
        state = service::query_state();
    } else {
        log_info("Service", "服务运行中，不重复安装、不弹 UAC");
    }
    // 运行中就不动它，也不弹 UAC。
    state = settle_state(state, 5000);
    log_info("Service", "落定后的状态：" + std::string(service::state_name(state)));

    // 宿主 exe 是不是还在：不在就得重新安装指向当前目录。
    if (Result<std::string> host = service::installed_binary_path(); ok(host)) {
        const std::string path = std::get<std::string>(host);
        if (!iequals(path, options.self_path) && !std::filesystem::exists(path_from_utf8(path))) {
            log_warn("Service", "服务宿主 exe 已丢失：" + path);
            std::printf("服务指向的可执行文件已丢失：%s\n", path.c_str());
            std::printf("是否重新安装服务并指向当前目录？(y/N) ");
            std::fflush(stdout);

            std::string answer;
            std::getline(std::cin, answer);
            if (!answer.empty() && (answer[0] == 'y' || answer[0] == 'Y')) {
                run_service_command("reinstall", options);
            } else {
                log_info("Service", "用户放弃重新安装");
            }
        }
    }

    return run_interactive(options, state);
}

}  // namespace

int dispatch_command(const std::vector<std::string>& args, const Options& options) {
    // 一次性命令不参与单实例：已经开着一个窗口时，别的脚本仍然要能停服务。
    if (!args.empty()) {
        if (args[0] == "service") {
            if (args.size() < 2 || !is_user_service_command(args[1])) {
                std::fprintf(stderr, "用法：service install | uninstall | start | stop\n");
                return exit_code(ErrorCode::InvalidArgument);
            }
            return run_service_command(args[1], options);
        }
        if (args.size() >= 2) {
            Session session;
            return run_business_command(args, session, options);
        }
        std::fprintf(stderr, "未知命令：%s\n\n", args[0].c_str());
        print_usage();
        return exit_code(ErrorCode::InvalidArgument);
    }

    // 双击：只留一个 CLI 窗口。
    SetConsoleTitleW(kConsoleTitle);
    HANDLE singleton = CreateMutexW(nullptr, TRUE, kSingletonMutex);
    if (singleton != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        activate_existing_window();
        CloseHandle(singleton);
        return 0;
    }

    const int code = bootstrap_and_run(options);

    if (singleton != nullptr) {
        ReleaseMutex(singleton);
        CloseHandle(singleton);
    }
    return code;
}

int run(const std::vector<std::string>& args, const Options& options) {
    if (!args.empty() && args[0] == "--elevated") {
        return run_elevated(args);  // 提权副本自己开日志器（在 run_elevated 里）
    }
    if (!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
        print_usage();
        return 0;
    }
    if (!args.empty() && (args[0] == "--version" || args[0] == "-v")) {
        print_version();
        return 0;
    }

    // 从这里开始都是真的干活，才值得写日志：--help / --version 不该在磁盘上
    // 留下任何东西。日志与 Service 共用同一个 <数据根>/log/fmt.log。
    std::unique_ptr<Logger> file_logger;
    if (Result<std::unique_ptr<Logger>> opened = open_cli_logger(options.data_root); ok(opened)) {
        file_logger = std::move(std::get<std::unique_ptr<Logger>>(opened));
        set_logger(file_logger.get());
    }

    std::string summary = "CLI 启动 v" + std::string(version::STRING) +
                          "，数据根：" + to_forward_slashes(options.data_root);
    if (args.empty()) {
        summary += "，交互模式";
    } else {
        summary += "，命令：" + args[0];
        if (args.size() > 1) {
            summary += " " + args[1];
        }
    }
    log_info("Cli", summary);
    note_service_root(options);

    const int code = dispatch_command(args, options);

    log_info("Cli", "CLI 退出，错误码 " + std::to_string(code));
    set_logger(nullptr);  // 先摘掉指针，再让 file_logger 析构
    return code;
}

}  // namespace fmt::cli
