#include "fmt/cli/cli.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

#include "fmt/common/string.hpp"
#include "fmt/core/app.hpp"
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
        "  fmt.exe service status       查询服务状态（不需要管理员权限）\n"
        "\n"
        "直接双击进入交互式命令行：\n"
        "  fmt> service status\n"
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
    // hello 的回复：服务是否因为我们的声明换了数据根。
    bool root_switched = false;
    std::string previous_root;
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
    const ipc::Response& hello_response = std::get<ipc::Response>(response);
    if (!hello_response.ok) {
        return hello_response.error;
    }

    session.root_switched = hello_response.data.value("switched", false);
    session.previous_root = hello_response.data.value("previous_root", std::string{});
    session.connected = true;

    log_info("Ipc", "已连接服务，数据根声明为：" + to_forward_slashes(options.data_root));
    return std::monostate{};
}

std::string join(const std::vector<std::string>& parts) {
    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            text += "、";
        }
        text += parts[i];
    }
    return text;
}

// ---- 双击第 1 步：数据根的检查与补齐 ----
//
// 只补缺失的目录与文件，已存在的一律不动；损坏的 JSON 只报告、绝不重置
// （冻结规则：JSON 损坏不能静默重置）。
//
// 必须在打开日志器**之前**调用：日志目录 log/ 也归这一步建，否则
// 「新建目录」的清单会少一个 log（日志器自己把它建掉了）。
// 打印出来的每一行同时收进 notes，日志器打开后再补记进日志。
void prepare_data_root(const Options& options, std::vector<std::string>* notes) {
    const PathManager paths{path_from_utf8(options.data_root)};
    const RootReport report = check_root(paths);
    const std::string root_text = to_forward_slashes(options.data_root);

    const auto emit = [notes](const std::string& line) {
        std::printf("  %s\n", line.c_str());
        if (notes != nullptr) {
            notes->push_back("数据根" + line);
        }
    };

    std::printf("数据根：%s\n", root_text.c_str());
    if (notes != nullptr) {
        notes->push_back("数据根检查：" + root_text);
    }

    Result<RootRepair> repaired = ensure_root(paths);
    if (!ok(repaired)) {
        const Error& error = *error_of(repaired);
        emit("无法补齐：" + code_string(error.code) + " " + error.message);
        return;  // 不阻断：服务自己启动时还会再试一次
    }

    const RootRepair& done = std::get<RootRepair>(repaired);
    if (!done.created_directories.empty()) {
        emit("新建目录：" + join(done.created_directories));
    }
    if (!done.created_files.empty()) {
        emit("新建文件：" + join(done.created_files));
    }
    if (done.created_directories.empty() && done.created_files.empty()) {
        emit("数据根完整");
    }

    for (const std::string& broken : report.broken_files) {
        emit("损坏（未自动修复）：" + broken);
    }
}

// ---- service status：查状态，不需要管理员权限，也就不该弹 UAC ----
int show_service_status() {
    const service::State state = service::query_state();
    const std::string name(service::state_name(state));

    std::printf("服务状态：%s\n", name.c_str());
    log_info("Cli", "service status：" + name);

    if (state == service::State::NotInstalled) {
        log_error("Cli", "service status：服务未安装（FMT-601）");
        std::fprintf(stderr, "错误码：%d\n", exit_code(ErrorCode::ServiceNotInstalled));
        return exit_code(ErrorCode::ServiceNotInstalled);
    }

    if (Result<std::string> host = service::installed_binary_path(); ok(host)) {
        const std::string path = to_forward_slashes(std::get<std::string>(host));
        std::printf("服务宿主：%s\n", path.c_str());
        log_info("Cli", "服务宿主：" + path);
    }
    if (Result<service::ServiceState> recorded = service::load_state(); ok(recorded)) {
        const std::string root = std::get<service::ServiceState>(recorded).current_root;
        if (!root.empty()) {
            std::printf("服务数据根：%s\n", root.c_str());
            log_info("Cli", "服务数据根：" + root);
        }
    }

    std::printf("错误码：0\n");
    return 0;
}

// 数据根/配置类的问题靠重装服务解决不了，别白弹一次 UAC。
bool is_data_root_problem(ErrorCode code) {
    switch (code) {
        case ErrorCode::JsonParseError:
        case ErrorCode::JsonWriteError:
        case ErrorCode::JsonUnsupportedVersion:
        case ErrorCode::ConfigError:
        case ErrorCode::DirectoryCreateFailed:
        case ErrorCode::StorageError:
        case ErrorCode::IoError:
        case ErrorCode::PathEscape:
            return true;
        default:
            return false;
    }
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
int run_interactive(const Options& options, service::State state, Session& session) {
    std::printf("FMT %.*s\n", static_cast<int>(version::STRING.size()), version::STRING.data());
    std::printf("%s\n", service_state_line(state).c_str());
    log_info("Cli", "进入交互循环，数据根：" + to_forward_slashes(options.data_root));

    bool blank_before_prompt = true;  // 横幅之后先空一行，输出不会和提示符挤在一起
    while (true) {
        if (blank_before_prompt) {
            std::printf("\n");
        }
        std::printf("fmt> ");
        std::fflush(stdout);

        std::string line;
        if (!std::getline(std::cin, line)) {
            std::printf("\n");
            break;  // Ctrl+Z 或输入结束
        }

        const std::vector<std::string> parts = split_command(line);
        if (parts.empty()) {
            blank_before_prompt = false;  // 光敲回车不再多空一行
            continue;
        }
        blank_before_prompt = true;

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
            if (parts.size() >= 2 && parts[1] == "status") {
                show_service_status();  // 查询不需要提权
                continue;
            }
            if (parts.size() < 2 || !is_user_service_command(parts[1])) {
                std::fprintf(stderr, "用法：service install | uninstall | start | stop | status\n");
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

// ---- 双击引导 ----
//
//   1. 数据根：检查完整性 + 补齐缺失（只补不缺）
//   2. 服务：未安装 -> 安装并启动；已安装未运行 -> 启动；运行中 -> 不动、不弹 UAC
//      启动失败 -> 先读 SCM 留下的失败编号；数据根/配置类问题不重装（重装也解决不了），
//                  其它情况重装一次（卸载 + 安装，一次 UAC）
//   3. 服务宿主 exe 丢失 -> 询问是否重装指向当前目录
//   4. 服务在跑就把数据根声明过去，然后进交互循环
service::State recover_from_start_failure(const Options& options, service::State current) {
    ErrorCode failure = ErrorCode::ServiceOperationFailed;
    bool known = false;

    if (Result<ErrorCode> reported = service::last_start_failure(); ok(reported)) {
        failure = std::get<ErrorCode>(reported);
        known = true;
    }

    const std::string reason = code_string(failure) + " " + std::string(default_message(failure));
    std::printf("服务启动失败：%s\n", reason.c_str());
    log_error("Service", "服务启动失败：" + reason);

    if (known && is_data_root_problem(failure)) {
        std::printf("这是数据根或配置的问题，重新安装服务解决不了；请先处理上面的错误\n");
        log_error("Service", "判定为数据根/配置问题，跳过重装");
        return current;
    }

    std::printf("尝试重新安装服务（卸载 + 安装，一次 UAC）\n");
    log_info("Service", "启动失败，尝试 reinstall");
    run_service_command("reinstall", options);

    const service::State after = settle_state(service::query_state(), 8000);
    if (after != service::State::Running) {
        std::printf("重新安装后服务仍未运行，请查看 log/fmt.log\n");
        log_error("Service", "reinstall 之后服务仍未运行");
    }
    return after;
}

// 服务宿主 exe 是不是还在：不在就得重装指向当前目录。
void check_service_host(const Options& options) {
    Result<std::string> host = service::installed_binary_path();
    if (!ok(host)) {
        return;
    }
    const std::string path = std::get<std::string>(host);
    if (iequals(path, options.self_path) || std::filesystem::exists(path_from_utf8(path))) {
        return;
    }

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

int bootstrap_and_run(const Options& options) {
    // 数据根已经在 run() 里补过了（那一步要在日志器之前做）。
    // 2. 服务状态
    service::State state = service::query_state();
    log_info("Service", "当前状态：" + std::string(service::state_name(state)));
    std::printf("服务状态：%s\n", std::string(service::state_name(state)).c_str());

    if (state == service::State::NotInstalled) {
        log_info("Service", "服务未安装 -> 安装并启动");
        run_service_command("install", options);  // 一次 UAC：装 + 启动
        state = settle_state(service::query_state(), 8000);
    } else if (state == service::State::Running) {
        log_info("Service", "服务运行中，不重复安装、不弹 UAC");
    } else {
        // 已安装但没在运行（已停止 / 正在停止 / 正在启动）：先尝试启动
        log_info("Service", "服务未在运行 -> 尝试启动");
        run_service_command("start", options);
        state = settle_state(service::query_state(), 8000);

        if (state != service::State::Running) {
            state = recover_from_start_failure(options, state);
        }
    }
    log_info("Service", "落定后的状态：" + std::string(service::state_name(state)));

    // 3. 宿主 exe
    check_service_host(options);

    // 4. 服务在跑就把数据根声明过去：这正是「数据根跟着 exe 走」。
    Session session;
    if (state == service::State::Running) {
        if (const Status status = ensure_connected(session, options); !ok(status)) {
            log_warn("Cli", "暂时无法把数据根声明给服务：" + error_of(status)->message);
        } else if (session.root_switched) {
            const std::string root_text = to_forward_slashes(options.data_root);
            std::printf("服务数据根已切换：%s -> %s\n", session.previous_root.c_str(),
                        root_text.c_str());
            log_info("Service", "数据根切换：" + session.previous_root + " -> " + root_text);
        } else {
            log_info("Service",
                     "服务数据根已经是：" + to_forward_slashes(options.data_root));
        }
    }

    return run_interactive(options, state, session);
}

}  // namespace

int dispatch_command(const std::vector<std::string>& args, const Options& options) {
    // 一次性命令不参与单实例：已经开着一个窗口时，别的脚本仍然要能停服务。
    if (!args.empty()) {
        if (args[0] == "service") {
            if (args.size() >= 2 && args[1] == "status") {
                return show_service_status();  // 查询不需要提权
            }
            if (args.size() < 2 || !is_user_service_command(args[1])) {
                std::fprintf(stderr, "用法：service install | uninstall | start | stop | status\n");
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

    // 双击：先把数据根补齐（含 log/），再开日志器——这样「新建目录」的清单是
    // 完整的六个，这些行也不会因为日志器还没开而丢掉。
    std::vector<std::string> root_notes;
    if (args.empty()) {
        prepare_data_root(options, &root_notes);
    }

    // 从这里开始都是真的干活，才值得写日志：--help / --version 不该在磁盘上
    // 留下任何东西。日志与 Service 共用同一个 <数据根>/log/fmt.log。
    std::unique_ptr<Logger> file_logger;
    if (Result<std::unique_ptr<Logger>> opened = open_cli_logger(options.data_root); ok(opened)) {
        file_logger = std::move(std::get<std::unique_ptr<Logger>>(opened));
        set_logger(file_logger.get());
    }

    for (const std::string& note : root_notes) {
        log_info("Cli", note);
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
