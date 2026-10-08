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

// 版本文本的**唯一来源**：横幅、--version、version 命令都走它。
nlohmann::json argument_envelope(const nlohmann::json& positional, bool dry_run, bool force) {    // 开关必须与 argv **同级**。以前这里写成 `args = positional; args["dry_run"] = true;`，
    // 而 positional 是数组——nlohmann 对数组用字符串下标会抛 type_error.305，
    // 未捕获就是 abort()：用户敲 `file delete a7.jpg` 时弹出的那个 Debug Error 就是它。
    nlohmann::json args = nlohmann::json::object();
    if (!positional.empty()) {
        args["argv"] = positional;
    }
    if (dry_run) {
        args["dry_run"] = true;
    }
    if (force) {
        args["force"] = true;
    }
    return args;
}

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

// 状态落定的兜底上限：正常由 SCM 的 dwWaitHint 决定，这只是最后一道闸。
constexpr int kSettleCapMs = 30000;

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

std::string banner_text() {
    return "File Manager Tool  v" + std::string(version::MAJOR) + "." +
           std::string(version::MINOR) + "  ( build  " + std::string(version::BUILD_DATE) + " )";
}

void print_version() { std::printf("%s\n", version_text().c_str()); }

// 命令总览：只列命令、不加描述；细节用 help <命令>。
void print_command_list() {
    std::printf("可用命令：\n");
    std::printf("  (service)  install  uninstall  start  stop  status\n");
    std::printf("  (bucket)   create  list  get  use  delete\n");
    std::printf("  (file)     upload  list  get  delete\n");
    std::printf("  (trash)    list  get  restore  delete\n");
    std::printf("  (help)     help [命令]\n");
    std::printf("  (version)  version                打印版本与构建日期\n");
    std::printf("  (exit)     exit  quit\n");
    std::printf("\n业务命令（服务端尚未实现，现在会返回 FMT-602）：\n");
    std::printf("  (share)    create  get  list  delete\n");
}

// help <命令>：某一组命令的详细说明。
bool print_command_help(const std::string& topic) {
    if (topic == "service") {
        std::printf(
            "service —— Windows 服务管理\n"
            "  install    安装并启动服务；需要管理员权限，弹一次 UAC\n"
            "  uninstall  停止并删除服务；需要管理员权限\n"
            "             不删除 repository / trash / config / data / log / temp\n"
            "  start      启动服务；需要管理员权限\n"
            "  stop       停止服务；需要管理员权限\n"
            "  status     查询服务状态；不需要管理员权限\n"
            "\n"
            "说明：启动类型为自动启动，运行账户为 LocalSystem；异常退出由 Windows\n"
            "      服务恢复策略自动重启（第一次 5 秒、第二次 10 秒、之后 30 秒）。\n");
        return true;
    }
    if (topic == "exit" || topic == "quit") {
        std::printf("exit / quit —— 退出命令行窗口\n");
        return true;
    }
    if (topic == "version") {
        std::printf(
            "version —— 打印程序名、版本与构建日期\n"
            "  与启动横幅、`--version` 共用同一份文本，不会各说一套。\n"
            "  不需要服务在运行，也不写任何磁盘内容：\n"
            "    fmt.exe version          一次性执行\n"
            "    fmt> version             窗口里执行\n");
        return true;
    }
    if (topic == "help") {
        std::printf("help [命令] —— 不带参数列出所有命令；带命令名看该命令的详细说明\n");
        return true;
    }
    if (topic == "bucket") {
        std::printf(
            "bucket —— 存储空间（Bucket 就是一个目录，没有独立 ID）\n"
            "  create <名称>   创建；第一个 Bucket 会自动成为当前 Bucket\n"
            "  list            列出所有 Bucket；当前的那个前面标 *\n"
            "  get <名称>      查看名称、是否当前、目录路径\n"
            "  use <名称>      切换当前 Bucket（只改 current_bucket，不动数据）\n"
            "  delete <名称>   移到回收站，名字变成 <名称>_<时间戳>；\n"
            "                  删的是当前 Bucket 时置空，不自动切换\n"
            "                  **桶里有文件时会先提醒**：删除后只能整体恢复这个桶，\n"
            "                  没法只恢复其中某个文件；一次性命令要加 --yes\n"
            "\n"
            "名称统一使用小写：create WORK 会建成 work（会提示你）；\n"
            "其余命令按名找桶时不区分大小写，找得到就按磁盘上的实际名字处理。\n"
            "回收站的桶级记录在 trash/<用户>/.original 里，回退用 trash restore。\n");
        return true;
    }
    if (topic == "file") {
        std::printf(
            "file —— 文件（当前用户在当前 Bucket 里的文件）\n"
            "  upload <来源> [文件名]   来源可以是 http:// 或 https:// 的 URL，也可以是本机路径。\n"
            "                           文件名省略时取来源的最后一段；重名不会自动改名，\n"
            "                           会提示换一个名字；相同内容（MD5 相同）会被拒绝，\n"
            "                           不重复入库。大小上限取 config.json 的 max_upload_size。\n"
            "                           文件名不能与文件标识同形（fmt-YYYYMMDD-N）：\n"
            "                           那会和 file_id 混淆，属保留形状（FMT-106）。\n"
            "  list                     列出当前 Bucket 的正常文件\n"
            "  get <file_id|文件名>     按文件名只查正常文件；按 file_id 连回收站里的\n"
            "                           也查得到（带 is_trash 与 trash_path）\n"
            "  delete <file_id|文件名>  软删除进回收站，file_id 不变\n"
            "                           （文件级回收站目前只能写、还不能从 trash 查回，待阶段 7）\n"
            "\n"
            "文件落在 repository/<用户>/<Bucket>/YYYY/MM/DD/ 下；上传先写 temp/，\n"
            "校验（大小上限、MD5、文件名）通过后才移动入库。\n"
            "名字与 file_id 的比较都不区分大小写（Windows 习惯）。\n"
            "网络下载走系统组件（WinHTTP + Schannel），支持 https，不需要 OpenSSL。\n");
        return true;
    }
    if (topic == "share") {
        std::printf(
            "share —— 分享（服务端尚未实现，现在返回 FMT-602）\n"
            "  create <file_id>    创建分享\n"
            "  get <share_id>      查看\n"
            "  list <file_id>      列出某个文件的分享\n"
            "  delete <share_id>   取消分享\n");
        return true;
    }
    if (topic == "trash") {
        std::printf(
            "trash —— 回收站（两类条目：文件级 [文件] 与桶级 [桶]，都会标出来）\n"
            "  list             列出回收站里的条目，标出是文件还是桶\n"
            "  get <标识>       查看单个条目：类型、标识、删除时间、路径、大小/文件数\n"
            "  restore <标识>   [文件] 按 file_id 回退到原 Bucket 的原位置；\n"
            "                   [桶]   回退整个 Bucket（名称可以是回收站里的名字，\n"
            "                          也可以是原桶名——同名只有一个时）。\n"
            "                   目标位置已有同名正常文件/Bucket 就拒绝，不覆盖、不改名；\n"
            "                   随桶一起删除的文件**只能整体恢复那个桶**，单独恢复会被拒绝。\n"
            "  delete <标识>    **永久删除，不可恢复**：删掉数据与记录。\n"
            "                   交互窗口里会先说明要删什么、再问一次；\n"
            "                   一次性命令必须加 --yes，例如\n"
            "                   fmt.exe trash delete lazy-fox_20261008012233 --yes\n"
            "\n"
            "标识可以是 file_id（文件）、回收站里的目录名（桶）或原名；\n"
            "命中多条会报候选，请用 file_id 或完整的回收站名指定。\n"
            "文件级条目记在 file.json 里（is_trash / trash_reason / deleted_at 是权威），\n"
            "数据收在 trash/<用户>/.files/ 下；桶级记录在 trash/<用户>/.original，\n"
            "目录名一律带删除时间戳，两者不会互相干扰。\n");
        return true;
    }

    std::fprintf(stderr, "没有 %s 的帮助；输入 help 查看命令列表\n", topic.c_str());
    return false;
}

void print_usage() {
    std::printf("%s\n\n", banner_text().c_str());
    std::printf("用法：fmt.exe [命令]\n");
    std::printf("  不带参数直接双击：创建/检查数据根、确保服务在运行，然后进入交互式命令行。\n\n");
    print_command_list();
    std::printf("\n退出码：0 成功  1 通用错误  2 参数错误  3 对象不存在  4 冲突\n");
    std::printf("        5 权限/访问  6 数据一致性  7 配置错误  8 Service 错误\n");
    std::printf("\n详细说明：help <命令>，例如 help service\n");
}

std::string service_state_line(service::State state) {
    return state == service::State::Running ? "Service Running..." : "Service Stopped...";
}

// 刚安装或刚启动时服务还在 START_PENDING：等它落定再显示状态，
// 否则横幅会在服务已经起来的情况下写 Service Stopped...
//
// 「还要等多久」听 SCM 的 dwWaitHint，而不是写死秒数：慢机器上写死会把
// 「还在启动」误判成「启动失败」，然后白弹一次 UAC 去重装。
service::State settle_state(service::State state, int cap_ms) {
    constexpr int kMinStepMs = 100;
    constexpr int kMaxStepMs = 2000;

    int waited = 0;
    while (waited < cap_ms && (state == service::State::StartPending ||
                               state == service::State::StopPending)) {
        Result<service::StatusInfo> info = service::query_status();
        if (!ok(info)) {
            break;
        }
        const service::StatusInfo& current = std::get<service::StatusInfo>(info);
        state = current.state;

        int step = static_cast<int>(current.wait_hint_ms);
        if (step < kMinStepMs) {
            step = kMinStepMs;
        }
        if (step > kMaxStepMs) {
            step = kMaxStepMs;
        }
        if (waited + step > cap_ms) {
            step = cap_ms - waited;
        }
        if (step <= 0) {
            break;
        }

        Sleep(static_cast<DWORD>(step));
        waited += step;
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
// **控制台不打印常规结果**（建了什么、完不完整）：那是日志的活，CLI 窗口只留
// 交互。只有异常（补不齐、文件损坏）才打到 stderr。
//
// 必须在打开日志器**之前**调用：日志目录 log/ 也归这一步建，否则
// 「新建目录」的清单会少一个 log（日志器自己把它建掉了）。
void prepare_data_root(const Options& options, std::vector<std::string>* notes) {
    const PathManager paths{path_from_utf8(options.data_root)};
    const RootReport report = check_root(paths);
    const std::string root_text = to_forward_slashes(options.data_root);

    const auto note = [notes](const std::string& line) {
        if (notes != nullptr) {
            notes->push_back("数据根" + line);
        }
    };

    note("检查：" + root_text);

    Result<RootRepair> repaired = ensure_root(paths);
    if (!ok(repaired)) {
        const Error& error = *error_of(repaired);
        note("无法补齐：" + code_string(error.code) + " " + error.message);
        std::fprintf(stderr, "数据根无法补齐：%s %s\n", code_string(error.code).c_str(),
                     error.message.c_str());
        return;  // 不阻断：服务自己启动时还会再试一次
    }

    const RootRepair& done = std::get<RootRepair>(repaired);
    if (!done.created_directories.empty()) {
        note("新建目录：" + join(done.created_directories));
    }
    if (!done.created_files.empty()) {
        note("新建文件：" + join(done.created_files));
    }
    if (done.created_directories.empty() && done.created_files.empty()) {
        note("完整");
    }

    for (const std::string& broken : report.broken_files) {
        note("损坏（未自动修复）：" + broken);
        std::fprintf(stderr, "数据根文件损坏（未自动修复）：%s\n", broken.c_str());
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

// 定义在下面（print_business_data 与 print_precheck 都要用它）
void print_trash_entry(const nlohmann::json& entry);

// 业务命令的结果按形状打印：Bucket 列表、单条信息、或服务给的 message。
// 服务端返回结构化数据，怎么展示放在 CLI 这一侧。
void print_business_data(const nlohmann::json& data) {
    if (!data.is_object()) {
        if (!data.is_null()) {
            std::printf("%s\n", data.dump(2).c_str());
        }
        return;
    }

    // 服务端的提示（例如「Bucket 名称统一使用小写」）单独一行先打出来。
    if (const auto note = data.find("note"); note != data.end() && note->is_string()) {
        std::printf("提示：%s\n", note->get<std::string>().c_str());
    }

    // 列表：buckets: [{name, is_current}, …]
    if (const auto items = data.find("buckets"); items != data.end() && items->is_array()) {
        for (const nlohmann::json& item : *items) {
            const std::string name = item.value("name", std::string{});
            const bool current = item.value("is_current", false);
            std::printf("%s%s%s\n", current ? "* " : "  ", name.c_str(),
                        current ? "  (当前)" : "");
        }
        std::printf("共 %zu 个 Bucket\n", items->size());
        return;
    }

    // 回收站列表：entries: [{type, id, name, …}, …]——**文件与桶都标出来**
    if (const auto items = data.find("entries"); items != data.end() && items->is_array()) {
        for (const nlohmann::json& item : *items) {
            const bool is_file = item.value("type", std::string{}) == "file";
            std::string line = std::string(is_file ? "  [文件]  " : "  [桶]    ") +
                               item.value("name", std::string{});
            if (is_file) {
                line += "（Bucket " + item.value("bucket", std::string{}) + "，" +
                        format_size(item.value("bytes", std::uintmax_t{0})) + "）";
            } else {
                if (item.value("files", std::size_t{0}) > 0) {
                    line += "（" + std::to_string(item.value("files", std::size_t{0})) +
                            " 个文件，" + format_size(item.value("bytes", std::uintmax_t{0})) + "）";
                }
                if (item.value("id", std::string{}) != item.value("name", std::string{})) {
                    line += "  ->  " + item.value("id", std::string{});
                }
            }
            std::printf("%s\n", line.c_str());
        }
        std::printf("共 %zu 项（%zu 个文件、%zu 个桶）\n", items->size(),
                    data.value("files", std::size_t{0}), data.value("buckets", std::size_t{0}));
        return;
    }

    // 单条回收站条目（trash get / restore / delete 的结果）
    if (data.contains("entry") && data["entry"].is_object()) {
        print_trash_entry(data["entry"]);
        if (const auto message = data.find("message");
            message != data.end() && message->is_string()) {
            std::printf("%s\n", message->get<std::string>().c_str());
        }
        return;
    }

    // 文件列表：files: [{file_id, file_name, size}, …]
    if (const auto items = data.find("files"); items != data.end() && items->is_array()) {
        for (const nlohmann::json& item : *items) {
            std::printf("  %s  %s\n", item.value("file_name", std::string{}).c_str(),
                        format_size(item.value("size", std::uintmax_t{0})).c_str());
        }
        std::printf("共 %zu 个文件\n", items->size());
        return;
    }

    // 单条文件信息：file_id + size
    if (data.contains("file_id") && data.contains("size")) {
        std::printf("文件：%s\n", data.value("file_name", std::string{}).c_str());
        std::printf("file_id：%s\n", data.value("file_id", std::string{}).c_str());
        std::printf("Bucket：%s\n", data.value("bucket", std::string{}).c_str());
        std::printf("类型：%s%s\n", data.value("file_type", std::string{}).c_str(),
                    data.value("extension", std::string{}).c_str());
        std::printf("大小：%s\n", format_size(data.value("size", std::uintmax_t{0})).c_str());
        std::printf("MD5：%s\n", data.value("md5", std::string{}).c_str());
        if (data.contains("path")) {
            std::printf("路径：%s\n", data.value("path", std::string{}).c_str());
        }
        if (data.contains("trash_path")) {
            std::printf("回收站路径：%s\n", data.value("trash_path", std::string{}).c_str());
        }
        if (data.value("is_trash", false)) {
            std::printf("状态：在回收站（%s）\n",
                        data.value("trash_reason", std::string{}).c_str());
        } else {
            std::printf("状态：正常\n");
        }
        return;
    }

    // 单条 Bucket 信息
    if (data.contains("bucket") && data.contains("is_current")) {
        std::printf("Bucket：%s\n", data.value("bucket", std::string{}).c_str());
        std::printf("当前：%s\n", data.value("is_current", false) ? "是" : "否");
        if (data.contains("path")) {
            std::printf("路径：%s\n", data.value("path", std::string{}).c_str());
        }
        return;
    }

    if (const auto message = data.find("message");
        message != data.end() && message->is_string()) {
        std::printf("%s\n", message->get<std::string>().c_str());
        return;
    }

    if (!data.empty()) {
        std::printf("%s\n", data.dump(2).c_str());
    }
}

// 破坏性操作里的一项回收站条目：**文件与桶都要标出来**（用户要求）。
void print_trash_entry(const nlohmann::json& entry) {
    const std::string type = entry.value("type", std::string{});
    const bool is_file = (type == "file");

    std::printf("[%s] %s\n", is_file ? "文件" : "桶", entry.value("name", std::string{}).c_str());
    std::printf("  标识：%s\n", entry.value("id", std::string{}).c_str());
    if (is_file) {
        std::printf("  Bucket：%s\n", entry.value("bucket", std::string{}).c_str());
        std::printf("  大小：%s\n", format_size(entry.value("bytes", std::uintmax_t{0})).c_str());
    } else if (entry.value("files", std::size_t{0}) > 0) {
        std::printf("  文件数：%zu\n", entry.value("files", std::size_t{0}));
    }
    if (!entry.value("deleted_at", std::string{}).empty()) {
        std::printf("  删除时间：%s\n", entry.value("deleted_at", std::string{}).c_str());
    }
    if (!entry.value("trash_path", std::string{}).empty()) {
        std::printf("  回收站路径：%s\n", entry.value("trash_path", std::string{}).c_str());
    }
    if (!entry.value("present", true)) {
        std::printf("  状态：数据已不存在\n");
    }
    if (!entry.value("restorable", true)) {
        std::printf("  可回退：否（%s）\n", entry.value("reason", std::string{}).c_str());
    }
}

// 破坏性操作的预检结果打印（**不走** print_business_data：那是给真实结果用的，
// 预检里的布尔字段不该被当成结果 dump 出来）。
void print_precheck(const nlohmann::json& data) {
    if (const auto message = data.find("message"); message != data.end() && message->is_string()) {
        std::printf("%s\n", message->get<std::string>().c_str());
    }

    // 歧义候选：把两条记录各自的 file_id 摊开，用户才知道该用哪个
    if (const auto items = data.find("candidates"); items != data.end() && items->is_array()) {
        std::printf("候选：\n");
        for (const nlohmann::json& item : *items) {
            std::printf("  %s（Bucket：%s，file_id %s）\n",
                        item.value("file_name", std::string{}).c_str(),
                        item.value("bucket", std::string{}).c_str(),
                        item.value("file_id", std::string{}).c_str());
        }
    }

    // 回收站条目的预检（回退 / 永久删除）
    if (data.contains("entry") && data["entry"].is_object()) {
        print_trash_entry(data["entry"]);
    }

    // bucket.delete 的预检：桶里有多少东西
    if (data.contains("has_content")) {
        std::printf("Bucket：%s\n", data.value("bucket", std::string{}).c_str());
        std::printf("当前：%s\n", data.value("is_current", false) ? "是" : "否");
        std::printf("文件数：%zu\n", data.value("files", std::size_t{0}));
        std::printf("占用：%s\n", format_size(data.value("bytes", std::uintmax_t{0})).c_str());
    }
}

// 预检与确认的结论。
//
// `exit_code` 是「不要继续」时要返回给系统的退出码——**预检自身的错误必须原样透出**：
// 文件不存在是 3、通信失败是 8，不能被「需要确认」的 2 盖掉，否则脚本会误读。
struct ConfirmOutcome {
    bool proceed = false;
    int exit_code = 0;
};

ConfirmOutcome refuse_with(ErrorCode code) {
    return ConfirmOutcome{false, exit_code(code)};
}

// 破坏性操作的统一流程：**先检查 → 说清楚冲突的具体对象 → 再确认**。
//
//   ① 发一次预检（dry_run，零副作用）：由服务端判定有没有要先说清楚的情况
//   ② 把情况原样打印（目标在哪个 Bucket、哪两条记录撞车、要永久删掉多少东西）
//   ③ 交互窗口问 y/N；一次性命令要求 --yes
//   ④ 用户同意之后才给真实请求带 force
//
// 所以在用户确认之前，**一个破坏性请求都不会发出去**。
ConfirmOutcome confirm_before_acting(const std::string& operation, const nlohmann::json& arguments,
                                     bool pre_confirmed, Session& session, const Options& options,
                                     bool interactive) {
    ipc::Request check;
    check.id = session.next_id++;
    check.op = operation;
    check.root = options.data_root;
    check.pid = GetCurrentProcessId();
    check.args = argument_envelope(arguments, /*dry_run=*/true);

    Result<ipc::Response> checked = session.client.call(check, ipc::kCommandTimeoutMs);
    if (!ok(checked)) {
        session.connected = false;
        std::fprintf(stderr, "预检失败：%s\n", error_of(checked)->message.c_str());
        return refuse_with(error_of(checked)->code);  // 通信类错误，不是「需要确认」
    }
    const ipc::Response& response = std::get<ipc::Response>(checked);
    if (!response.ok) {
        print_failure(response.error);  // 预检就错了（不存在、已在回收站…）直接报出来
        return refuse_with(response.error.code);
    }

    print_precheck(response.data);

    if (response.data.value("blocked", false)) {
        // 例如「名字与 file_id 撞车」：y 无法表达删哪一个，必须让用户改用 file_id。
        std::fprintf(stderr, "这项操作不能靠确认解决，请按上面的提示指定具体对象\n");
        return refuse_with(ErrorCode::InvalidArgument);
    }
    if (!response.data.value("needs_confirm", false)) {
        return ConfirmOutcome{true, 0};  // 没有什么要先说清楚的，照做
    }

    if (!pre_confirmed) {
        if (!interactive) {
            std::fprintf(stderr, "该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行\n");
            log_warn("Cli", "拒绝未确认的破坏性操作：" + operation);
            return refuse_with(ErrorCode::ConfirmRequired);
        }
        std::printf("确认执行？(y/N) ");
        std::fflush(stdout);

        std::string answer;
        std::getline(std::cin, answer);
        if (answer.empty() || (answer[0] != 'y' && answer[0] != 'Y')) {
            log_info("Cli", "用户取消了：" + operation);
            std::printf("已取消\n");
            return ConfirmOutcome{false, 0};  // 用户主动取消，不是错误
        }
    }
    return ConfirmOutcome{true, 0};
}

int run_business_command(const std::vector<std::string>& parts, Session& session,
                         const Options& options, bool interactive) {
    const std::string operation = parts[0] + "." + parts[1];

    // 位置参数。--yes 是本地开关，不发给服务。
    nlohmann::json arguments = nlohmann::json::array();
    bool confirmed = false;
    for (std::size_t i = 2; i < parts.size(); ++i) {
        if (parts[i] == "--yes" || parts[i] == "-y") {
            confirmed = true;
            continue;
        }
        arguments.push_back(parts[i]);
    }

    if (const Status status = ensure_connected(session, options); !ok(status)) {
        log_error("Cli", "命令 " + operation + " 无法连接服务：" + error_of(status)->message);
        print_failure(*error_of(status));
        return exit_code(error_of(status)->code);
    }

    // 破坏性操作：先检查、说清楚、再确认。
    // bucket.delete 也在内——桶里有东西时要提醒「之后只能整体恢复这个桶」。
    const bool destructive = (operation == "file.delete" || operation == "trash.delete" ||
                              operation == "bucket.delete");
    if (destructive) {
        const ConfirmOutcome outcome =
            confirm_before_acting(operation, arguments, confirmed, session, options, interactive);
        if (!outcome.proceed) {
            return outcome.exit_code;  // 预检的错误码原样透出；用户取消是 0
        }
    }

    ipc::Request request;
    request.id = session.next_id++;
    request.op = operation;
    request.root = options.data_root;
    request.pid = GetCurrentProcessId();

    // 位置参数与开关走**同一个**信封构造：预检与真实请求因此不可能各错一处。
    request.args = argument_envelope(arguments, /*dry_run=*/false, confirmed || destructive);

    log_info("Cli", "命令 " + operation + " 已发送（id " + std::to_string(request.id) + "）");

    // 上传是长任务，用单独的长超时；普通命令仍是 30 秒。
    const int timeout_ms =
        operation == "file.upload" ? ipc::kUploadTimeoutMs : ipc::kCommandTimeoutMs;

    Result<ipc::Response> response = session.client.call(request, timeout_ms);
    if (!ok(response)) {
        session.connected = false;
        const Error& error = *error_of(response);
        log_error("Cli", "命令 " + operation + " 通信失败：" + error.message);
        print_failure(error);

        // 超时 ≠ 没成功：服务端可能还在下载、甚至已经入库。必须说清楚，
        // 否则用户会以为文件没进去，然后重复上传。
        if (error.code == ErrorCode::ServiceOperationFailed &&
            error.message.find("超时") != std::string::npos) {
            std::fprintf(stderr,
                         "提示：等待服务响应超时。服务端可能仍在处理，稍后用 file list 确认；\n"
                         "      也可以查看 log/fmt.log。\n");
        }
        return exit_code(error.code);
    }

    const ipc::Response& value = std::get<ipc::Response>(response);
    if (value.ok) {
        log_info("Cli", "命令 " + operation + " 执行成功（错误码 0）");
        print_business_data(value.data);
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
    std::printf("%s\n", banner_text().c_str());
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
            if (parts.size() >= 2) {
                print_command_help(parts[1]);  // help service 看某一组命令的详情
            } else {
                print_command_list();
            }
            continue;
        }
        if (head == "version" || head == "--version" || head == "-v") {
            std::printf("%s\n", version_text().c_str());
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

        run_business_command(parts, session, options, true);
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

    const service::State after = settle_state(service::query_state(), kSettleCapMs);
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

// 双击引导里**唯一**的进度提示。
//
// 数据根体检的细节（建了哪些目录、服务状态）只进日志，不刷控制台——这是用户
// 明确要求过的。但「等服务落定 + 连接并声明数据根」这段可能要几秒且完全静默，
// 窗口看着像卡住，所以留这一行：它是提示，不是细节。
void announce_initializing() {
    std::printf("\n正在初始化配置...\n\n");
    std::fflush(stdout);  // 后面就要开始等了，必须立刻显出来
    log_info("Cli", "正在初始化配置（等服务落定、连接并声明数据根）");
}

int bootstrap_and_run(const Options& options) {
    // 数据根已经在 run() 里补过了（那一步要在日志器之前做）。
    // 2. 服务状态
    service::State state = service::query_state();
    log_info("Service", "当前状态：" + std::string(service::state_name(state)));

    const service::State initial = state;
    const bool must_settle = (initial != service::State::Running);
    // 未安装时装完不额外走一次「启动失败诊断」：一次 UAC 已经花掉了，
    // 再自动重装就是第二次，不能不打商量。
    const bool allow_recovery =
        (initial != service::State::Running && initial != service::State::NotInstalled);

    if (initial == service::State::NotInstalled) {
        log_info("Service", "服务未安装 -> 安装并启动");
        run_service_command("install", options);  // 一次 UAC：装 + 启动
    } else if (initial == service::State::Running) {
        log_info("Service", "服务运行中，不重复安装、不弹 UAC");
    } else {
        // 已安装但没在运行（已停止 / 正在停止 / 正在启动）：先尝试启动
        log_info("Service", "服务未在运行 -> 尝试启动");
        run_service_command("start", options);
    }

    // 提权输出已经打完，接下来这一段才开始等待：提示打在两者之间。
    announce_initializing();

    if (must_settle) {
        state = settle_state(service::query_state(), kSettleCapMs);
        if (state != service::State::Running && allow_recovery) {
            state = recover_from_start_failure(options, state);
        }
    }
    log_info("Service", "落定后的状态：" + std::string(service::state_name(state)));

    // 3. 宿主 exe
    check_service_host(options);

    // 4. 服务在跑就把数据根声明过去：这正是「数据根跟着 exe 走」。
    //    换根这种例行状态变化只进日志，不往控制台刷（用户要看就看
    //    service status，它会把「服务数据根」打出来）。
    Session session;
    if (state == service::State::Running) {
        if (const Status status = ensure_connected(session, options); !ok(status)) {
            log_warn("Cli", "暂时无法把数据根声明给服务：" + error_of(status)->message);
        } else if (session.root_switched) {
            log_info("Service", "数据根切换：" + session.previous_root + " -> " +
                                    to_forward_slashes(options.data_root));
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
        if (args[0] == "help") {
            if (args.size() >= 2) {
                return print_command_help(args[1]) ? 0 : exit_code(ErrorCode::InvalidArgument);
            }
            print_command_list();
            return 0;
        }
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
            return run_business_command(args, session, options, false);
        }
        std::fprintf(stderr, "未知命令：%s\n\n", args[0].c_str());
        print_usage();
        return exit_code(ErrorCode::InvalidArgument);
    }

    // 双击：只留一个 CLI 窗口。
    SetConsoleTitleW(kConsoleTitle);
    HANDLE singleton = CreateMutexW(nullptr, TRUE, kSingletonMutex);
    if (singleton != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已经有一个 FMT 窗口：把它拉到前面，本次静默退出（控制台不加噪音，
        // 但日志里要看得出来「这次点击为什么什么都没发生」）。
        const bool activated = activate_existing_window();
        log_info("Cli", activated ? "已有 FMT 窗口，已激活它并退出（单实例）"
                                  : "已有 FMT 窗口，未能把它置前，直接退出（单实例）");
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
    if (!args.empty() && args[0] == "version") {
        // 与 --version 同源：不需要服务、不碰磁盘（所以放在开日志器之前）
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

// 版本文本的**唯一来源**：横幅、--version、version 命令都走它。
// 必须定义在匿名命名空间**外面**（banner_text 在里面，但那里面的名字对外不可见，
// 声明在 cli.hpp 里的函数不能跟着进去——否则链接期找不到符号）。
std::string version_text() { return banner_text(); }

}  // namespace fmt::cli
