#include "fmt/cli/cli.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "fmt/common/hash.hpp"
#include "fmt/common/string.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/app.hpp"
#include "fmt/core/path.hpp"
#include "fmt/core/path_manager.hpp"
#include "fmt/ipc/pipe.hpp"
#include "fmt/service/service.hpp"
#include "fmt/user/user.hpp"
#include "fmt/user/user_service.hpp"
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

// 环境变量开关：值为 "1" / "true" / "yes" 视为开。用 _dupenv_s 避免 C4996。
bool env_flag(const char* name) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return false;
    }
    const std::string text = to_lower(trim(value));
    free(value);
    return text == "1" || text == "true" || text == "yes";
}

void log(LogLevel level, std::string_view module, const std::string& message) {    if (g_logger != nullptr) {
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

// 日志清理的默认预算：与 cleanup_logs 的默认值同源，`log clean --keep` 可以覆盖。
constexpr std::uintmax_t kLogBudget = kDefaultLogBudgetBytes;

// log/ 里现有多少个文件、共占多少（只统计普通文件，别的目录/文件不动）。
struct LogDirectoryInfo {
    bool exists = false;
    std::size_t files = 0;
    std::uintmax_t bytes = 0;
};

LogDirectoryInfo inspect_log_directory(const std::filesystem::path& log_directory) {
    LogDirectoryInfo info;
    std::error_code code;
    if (!std::filesystem::is_directory(log_directory, code)) {
        return info;
    }
    info.exists = true;
    for (const auto& entry : std::filesystem::directory_iterator(log_directory, code)) {
        if (code) {
            break;
        }
        std::error_code type_code;
        if (!entry.is_regular_file(type_code)) {
            continue;
        }
        ++info.files;
        info.bytes += entry.file_size(type_code);
        type_code.clear();
    }
    return info;
}

// `--keep` 的大小写法：10485760 / 8MB / 512KB / 1GB（1KB = 1024 字节）。
// 与 config 的 max_upload_size 用同一套写法与同一套单位，免得用户记两套。
bool parse_log_budget(const std::string& text, std::uintmax_t* bytes) {
    if (text.empty() || bytes == nullptr) {
        return false;
    }

    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
        ++digits;
    }
    if (digits == 0) {
        return false;
    }

    const std::string value_text = text.substr(0, digits);
    const std::string unit = to_lower(text.substr(digits));
    std::uintmax_t multiplier = 1;
    if (unit.empty() || unit == "b") {
        multiplier = 1;
    } else if (unit == "kb") {
        multiplier = 1024;
    } else if (unit == "mb") {
        multiplier = 1024 * 1024;
    } else if (unit == "gb") {
        multiplier = 1024ull * 1024 * 1024;
    } else {
        return false;
    }

    const unsigned long long value = std::strtoull(value_text.c_str(), nullptr, 10);
    *bytes = static_cast<std::uintmax_t>(value) * multiplier;
    return true;
}

namespace {

constexpr wchar_t kSingletonMutex[] = L"Local\\FMT.CLI.v1";
constexpr wchar_t kConsoleTitle[] = L"FMT";

// 服务刚被 service start 拉起来时监听还没就绪，连接要给它一点时间。
constexpr int kConnectWaitMs = 5000;

// 状态落定的兜底上限：正常由 SCM 的 dwWaitHint 决定，这只是最后一道闸。
constexpr int kSettleCapMs = 30000;

// 服务当前数据根与本进程不同时，点明服务侧日志写在哪里：// 两个进程各写自己数据根下的 log/fmt.log（根一样时才是同一个文件）。
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

}  // namespace

// 隐藏输入口令：控制台上逐字符读、回显 `*`；stdin 被重定向时按行读。
//
// 前导的 `\n` 是刻意的：调用点刚用 std::getline 读过上一行（用户名 / 子命令），
// 而 `_getch()` 绕过了 C 运行时的行缓冲，不清掉残留的换行就会把提示符顶到上一行。
//
// 定义在 `fmt::cli` 层（**不进匿名名字空间**）：它同时是 cli.hpp 里的公开接口，
// 放进匿名名字空间会与那份声明形成两个不同的实体，调用点就"重载不明确"了。
bool read_password_masked(const char* prompt, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    if (prompt == nullptr) {
        prompt = "";
    }

    // 「有没有屏幕」看的是 **stdin 的类型**，不是"能不能打开 CONIN$"。
    //
    // 真踩过：stdin 被重定向成管道（脚本、自动化测试）时，CONIN$ 照样能打开成功
    // ——它打开的是**当前控制台**，跟被重定向的 stdin 没关系。于是代码跑去读真控制台，
    // 而脚本把口令喂在管道里，两边永远等不到对方：CLI 卡死（真机表现就是"敲了没反应"）。
    const HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    const DWORD stdin_type =
        (stdin_handle == nullptr || stdin_handle == INVALID_HANDLE_VALUE)
            ? FILE_TYPE_UNKNOWN
            : GetFileType(stdin_handle);
    const bool has_console = (stdin_type == FILE_TYPE_CHAR);

    HANDLE input = INVALID_HANDLE_VALUE;
    if (has_console) {
        input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                            nullptr);
    }
    if (input == INVALID_HANDLE_VALUE) {
        // stdin 被重定向：没有"屏幕"可言，按行读——明文本来也就不会回显到任何地方。
        std::printf("%s", prompt);
        std::fflush(stdout);
        std::string line;
        if (!std::getline(std::cin, line)) {
            return false;
        }
        *out = line;
        return true;
    }

    std::printf("\n%s", prompt);
    std::fflush(stdout);
    while (true) {
        INPUT_RECORD record = {};
        DWORD read = 0;
        if (!ReadConsoleInputW(input, &record, 1, &read) || read == 0) {
            break;
        }
        if (record.EventType != KEY_EVENT || record.Event.KeyEvent.bKeyDown == FALSE) {
            continue;
        }
        const wchar_t ch = record.Event.KeyEvent.uChar.AsciiChar;
        if (ch == L'\r' || ch == L'\n') {
            break;
        }
        if (ch == L'\b' || ch == 127) {  // 退格：删一个字符并把星号擦掉
            if (!out->empty()) {
                out->pop_back();
                std::printf("\b \b");
                std::fflush(stdout);
            }
            continue;
        }
        if (ch == 3) {  // Ctrl+C：放弃本次输入（控制台不会自己抛信号给我们）
            CloseHandle(input);
            std::printf("\n");
            return false;
        }
        if (ch == 0) {  // 方向键 / F1 之类的功能键：忽略
            continue;
        }
        if (out->size() < 256) {
            out->push_back(static_cast<char>(ch));
            std::printf("*");
            std::fflush(stdout);
        }
    }
    CloseHandle(input);
    std::printf("\n");
    std::fflush(stdout);
    return true;
}

// 改完 config/server.json 之后把服务重启起来（定义见文件后段）。

namespace {

std::string banner_text() {
    return "File Manager Tool  v" + std::string(version::STRING) + "  ( build  " + std::string(version::BUILD_DATE) + " )";
}

void print_version() { std::printf("%s\n", version_text().c_str()); }

// 命令总览：只列命令、不加描述；细节用 help <命令>。
// 顺序按「数据操作 → 配置 → 帮助/版本 → 退出」分组，而不是按命令名排序。
void print_command_list() {
    std::printf("可用命令：\n");
    std::printf("  (login)    login [用户名]  logout   登录 / 退出登录（业务命令要先登录）\n");
    std::printf("  (service)  install  uninstall  start  stop  reinstall  status\n");
    std::printf("  (user)     create  get  update password  delete  token renew\n");
    std::printf("  (bucket)   create  list  get  use  delete\n");
    std::printf("  (file)     upload  list  get  delete\n");
    std::printf("  (upload)   start  status  pause  resume  cancel  list  wait\n");
    std::printf("  (trash)    list  get  restore  delete  empty\n");
    std::printf("  (share)    create  get  list  delete\n");
    std::printf("  (config)   list  set\n");
    std::printf("  (log)      clean                   清理日志（启动时也会自动兜底）\n");
    std::printf("  (help)     help [命令]\n");
    std::printf("  (version)  version                打印版本与构建日期\n");
    std::printf("  (exit)     exit  quit\n");
}

// help <命令>：某一组命令的详细说明。
bool print_command_help(const std::string& topic) {
    if (topic == "login") {
        std::printf(
            "login / logout —— 登录与退出登录（业务命令要先登录）\n"
            "  login [用户名]   不带用户名就用账号里那个；回车后隐藏输入密码（回显 *）\n"
            "  logout           退出登录；之后的业务命令需要重新登录\n"
            "\n"
            "为什么需要登录：业务数据（文件/桶/回收站/分享/配置与改密删号）都要先证明你是机主。\n"
            "不需要登录的：service（装/卸/启/停/重装/状态）、log clean、help、version、\n"
            "user create（还没有账号时得先能建）、user update password（自己会问当前密码）、\n"
            "user delete（忘记密码时的唯一出路）。其余 user 子命令（get / token renew）要登录。\n"
            "\n"
            "登录态**只在本次窗口的内存里**：窗口一关就要重新登录。\n"
            "口令明文不出本进程：CLI 先取口令盐、本地算摘要，只把摘要交给服务比对。\n");
        return true;
    }
    if (topic == "user") {
        std::printf(
            "user —— 账号（一个数据根只有一个账号）\n"
            "  create <用户名>   建账号。随后提示输入密码并再输一次确认；\n"
            "                    输入时不回显，只显示 *。\n"
            "                    用户名只允许字母、数字与 _ . -（它同时是目录名）。\n"
            "                    已有账号时拒绝重建——要重建先 user delete。\n"
            "  get               看账号信息（用户名 / 用户 ID / 创建时间 / 当前 Bucket /\n"
            "                    密码是否还能改 / token 的有效期）。\n"
            "                    不打印口令哈希与 token。\n"
            "  update password   改密码：先输入当前密码，再输两次新密码。\n"
            "                    密码只能修改一次；已经改过就提示删除用户重新创建。\n"
            "  token renew       换发访问 token（要密码）。token 有时效，过期后 HTTP\n"
            "                    一律 401（FMT-019），这条是唯一的续期路径：换完旧 token\n"
            "                    立刻失效，且不打乱「密码只能改一次」。\n"
            "                    有效期由 `config set token_ttl 7d` 决定（默认 7 天）。\n"
            "  delete            永久删除这个数据根的全部业务数据（账号、令牌、桶里的文件、\n"
            "                    回收站历史），只保留 fmt.exe 与 log/。\n"
            "                    必须先输入正确密码，再逐字输入 delete 才动手。\n"
            "                    不进回收站、无法撤销。\n"
            "\n"
            "口令明文不会离开本进程：CLI 先算出摘要/应答值再发给服务，所以数据文件与\n"
            "命名管道里都看不到明文。\n");
        return true;
    }
    if (topic == "service") {
        std::printf(
            "service —— Windows 服务管理\n"
            "  install    安装并启动服务；需要管理员权限，弹一次 UAC\n"
            "  uninstall  停止并删除服务；需要管理员权限\n"
            "             不删除 repository / trash / config / data / log / temp\n"
            "  start      启动服务；需要管理员权限\n"
            "  stop       停止服务；需要管理员权限\n"
            "  reinstall  一次 UAC 里做完「卸载 → 按当前这个 exe 重新安装并启动」；\n"
            "             需要管理员权限。两个用途：\n"
            "               ① 用新 exe 更新服务——直接运行新 exe 的这条命令即可，\n"
            "                  它会先停掉旧宿主（解开文件占用）、再指向自己并启动；\n"
            "               ② 修复宿主 exe 被移动或删除。\n"
            "             业务数据一个都不动（repository / trash / config / data / log）。\n"
            "             注意：宿主会变成「你运行的那一份 exe」；数据根仍由连上来的\n"
            "             CLI 声明（见窗口横幅上的「数据根：…」）。\n"
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
            "                  桶里有文件时会先提醒：删除后只能整体恢复这个桶，\n"
            "                  没法只恢复其中某个文件；一次性命令要加 --yes\n"
            "\n"
            "名称统一使用小写：create WORK 会建成 work（会提示你）；\n"
            "其余命令按名找桶时不区分大小写，找得到就按磁盘上的实际名字处理。\n"
            "回收站的桶级记录在 trash/<用户>/.original 里，回退用 trash restore。\n");
        return true;
    }
    if (topic == "upload") {
        std::printf(
            "upload —— 大文件分片上传（普通文件仍用 file upload，single request）\n"
            "  start <本地文件> [文件名] [--chunk-size 8MB] [--wait]\n"
            "                           建分片会话并让服务后台传；--wait 显示实时进度条\n"
            "  status <upload_id>       查看进度、缺少的分片与错误；verified 是已记录 MD5 的分片数\n"
            "  pause <upload_id>        当前分片写完后暂停（算源文件 MD5 时也会尽快停）\n"
            "  resume <upload_id>       从缺少的分片继续；服务重启后也可用\n"
            "  cancel <upload_id>       取消并清理临时分片\n"
            "  list [--all]             列出上传记录；默认把同 file_id 的历史合并成一行\n"
            "  wait <upload_id>         持续显示进度条、速度、ETA 直到完成\n"
            "\n"
            "每个分片写入时记录 MD5，拼接时逐片校验；坏片会删除并提示 resume 重传。\n"
            "上限：max_large_upload_size；分片默认取 config.json 的 upload_chunk_size。\n"
            "服务重启后会话与已传分片仍在，upload resume 继续。\n");
        return true;
    }
    if (topic == "file") {
        std::printf(
            "file —— 文件（当前用户在当前 Bucket 里的文件）\n"
            "  upload <来源> [文件名]   来源可以是 http:// 或 https:// 的 URL，也可以是本机路径。\n"
            "                           文件名省略时取来源的最后一段。判重只看当前 Bucket，\n"
            "                           而且先看内容：桶内已有相同内容（不管名字同不同）→\n"
            "                           报「文件已存在」（FMT-304），不要求改文件名；\n"
            "                           内容不同而文件名相同 → 报「文件名重复」（FMT-105），\n"
            "                           提示修改文件名；两者都不同就正常入库。\n"
            "                           别的 Bucket 里的同名/同内容与本桶无关。\n"
            "                           大小上限取 config.json 的 max_upload_size。\n"
            "                           文件名不能与文件标识同形（fmt-YYYYMMDD-N）：\n"
            "                           那会和 file_id 混淆，属保留形状（FMT-106）。\n"
            "  list [--sort name|size|id] [--search 关键字] [--page N --page-size M]\n"
            "                           列出当前 Bucket 的正常文件；默认按名字，\n"
            "                           size 大的在前，id 是入库顺序。\n"
            "                           --search 按文件名/file_id 做不区分大小写的子串匹配；\n"
            "                           --page-size 给了才分页（0 = 一次给全），\n"
            "                           顺序固定为「过滤 -> 排序 -> 分页」，翻页不会漏或重。\n"
            "  get <file_id|文件名>     按文件名查当前 Bucket 的正常文件（同名的在别的桶时\n"
            "                           会告诉你在哪个桶）；按 file_id 全局查，连回收站里的\n"
            "                           也查得到（带 is_trash 与 trash_path）\n"
            "  delete <file_id|文件名|upload_id>\n"
            "                           软删除进回收站，file_id 不变；之后用\n"
            "                           trash list / trash restore 找回来。按名字只在当前\n"
            "                           Bucket 内生效；要删别的桶里的那份请用 file_id（会先确认）。\n"
            "                           给 completed 的 upload_id 也可以，服务会解析到\n"
            "                           对应的 file_id；未完成的上传请先 upload cancel。\n"
            "\n"
            "文件落在 repository/<用户>/<Bucket>/YYYY/MM/DD/ 下；上传先写 temp/，\n"
            "校验（大小上限、MD5、文件名）通过后才移动入库。\n"
            "名字与 file_id 的比较都不区分大小写（Windows 习惯）。\n"
            "网络下载走系统组件（WinHTTP + Schannel），支持 https，不需要 OpenSSL。\n");
        return true;
    }
    if (topic == "share") {
        std::printf(
            "share —— 分享（把正常文件开放成一条可撤销的链接）\n"
            "  create <file_id|文件名>  创建分享：默认 20 次、7 天过期；返回 share_id\n"
            "                          与完整分享链接（http://host:port/api/share/<id>/download，\n"
            "                          地址取自 config/server.json，不看 HTTP 服务有没有开）\n"
            "  get <share_id>      查看它的全部信息：文件与 Bucket、分享链接、下载次数\n"
            "                      （用满时写「（下载次数已满）」）、到期时间（过期时写\n"
            "                      「（已过期）」）、状态与一句话说明；链接不可用时也会给出，\n"
            "                      但标注「当前不可下载，链接已失效」。\n"
            "                      过期 / 次数用尽 / 已撤销 / 关联文件在回收站都如实报告，\n"
            "                      不伪装成不存在（只有 share_id 完全查不到才是 FMT-500）\n"
            "  list [<file_id|文件名>]  列出分享（最近创建的在前）：不带参数就列全部，\n"
            "                      一条一行：文件名（当前桶）、下载次数、状态、到期；\n"
            "                      次数用满/已过期时直接写「[下载次数已满]」「[下载已过期]」\n"
            "                      （两个原因可能同时成立）。这里不打印分享链接——\n"
            "                      链接用 `share get <share_id>` 看，列表保持一屏能扫完\n"
            "  delete <share_id>   撤销分享（删记录）\n"
            "\n"
            "约束（开发文档 §50）：分享不得绕过文件状态——文件进回收站或被删掉，\n"
            "分享立刻失效（FMT-503）。share_id 是 12 位随机十六进制，本身就是访问凭证。\n"
            "公开下载端点已实现（提交 d3aeb3d）：GET /api/share/<share_id>/download\n"
            "不要 token（链接本身就是凭证）、先记账再放行，可直接把链接交给别人。\n");
        return true;
    }
    if (topic == "config") {
        std::printf(
            "config —— 配置（config/config.json 与 config/server.json）\n"
            "  list [--show-token]           看当前生效的配置，两个文件按顺序分开输出：\n"
            "                                config.json 的字段在前，接着是网络服务的\n"
            "                                「网络服务 enabled / 服务地址 host / 服务端口 port」；\n"
            "                                默认不打印访问 token（省得暴露在屏幕、日志、\n"
            "                                工单与截图里）；要它加 --show-token，\n"
            "                                或直接读数据根下的 data/user.json\n"
            "  set max_upload_size <大小>     改上传大小上限；大小可写 10485760，\n"
            "                                也可写 10MB / 512KB / 1GB（1KB = 1024 字节）\n"
            "  set server_enabled <true|false>  开/关 HTTP 网络服务（config/server.json）\n"
            "  set server_host <地址>         监听地址（默认 localhost，只监听本机）\n"
            "  set server_port <1-65535>      监听端口（默认 4122）\n"
            "\n"
            "server_* 三项改完会自动重启服务（已经提权就直接重装，否则弹一次 UAC；\n"
            "提权被取消时会提示你手动执行 service reinstall）——因为 server.json\n"
            "只在服务启动时读一次。\n"
            "其余项（current_user / size_unit / language / version）只读。\n"
            "current_bucket 用 bucket use 切换，不走 config。\n");
        return true;
    }
    if (topic == "log") {
        std::printf(
            "log —— 日志文件（<数据根>/log/ 下的 fmt.log 与 error.log，各留一代 .1）\n"
            "  clean [--keep 8MB] [--dry-run]\n"
            "      清理日志：先把轮转备份（fmt.log.1 / error.log.1）删掉；\n"
            "      仍超过保留上限就把活动文件裁成最新的一段，并在文件顶部留一行\n"
            "      「日志已裁剪：保留最近 N 字节」的说明，断点看得见。\n"
            "      没超过上限时一个字节都不动。\n"
            "      --keep 8MB   改保留上限（默认 8MB；也可写 8388608 / 512KB / 1GB）\n"
            "      --dry-run    只报告会删什么、会裁什么、能释放多少，磁盘一个字节都不动\n"
            "\n"
            "这条命令本地执行、不需要服务（日志是每个数据根各自的本地文件）。\n"
            "每次启动 CLI / 服务时也会用同一套规则自动兜底清理一次，并把结果写进日志；\n"
            "所以正常情况下你不用手动敲它——需要用它的场合是「想看一眼现在多大」。\n");
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
            "                   目标位置已有同名正常文件就拒绝，不覆盖、不改名；\n"
            "                   随桶一起删除的文件只能整体恢复那个桶，单独恢复会被拒绝。\n"
            "                   目标 Bucket 已存在时（例如删桶之后先单独回退过桶里的某个文件，\n"
            "                   回退文件会把那个桶目录建出来）不是回退失败：桶里的文件会合并\n"
            "                   进现有桶——不覆盖、不改名，先说明搬几个文件再要一次确认\n"
            "                   （一次性命令加 --yes）。现有桶里有同名文件就一个都不搬，\n"
            "                   会列出冲突的名字（改完名再合并）。\n"
            "  delete <标识>     永久删除，不可恢复：删掉数据与记录。\n"
            "                   交互窗口里会先说明要删什么、再问一次；\n"
            "                   一次性命令必须加 --yes，例如\n"
            "                   fmt.exe trash delete lazy-fox_20261008012233 --yes\n"
            "  empty             清空整个回收站，不可恢复：两级条目一次全删。\n"
            "                   同样先说明「几项、多少」再问一次；一次性命令加 --yes。\n"
            "\n"
            "标识可以是 file_id（文件）、回收站里的目录名（桶）或原名；\n"
            "命中多条会报候选，请用 file_id 或完整的回收站名指定。\n"
            "文件级条目记在 file.json 里（is_trash / trash_reason / deleted_at 是权威），\n"
            "数据收在 trash/<用户>/.files/ 下；桶级记录在 trash/<用户>/.original，\n"
            "目录名一律带删除时间戳，两者不会互相干扰。\n");
        return true;
    }

    // `help fmt`：用户容易把程序名当成命令（真机反馈：有人在提示符里敲 `fmt -u lazy -p`）。
    // 这里不报"没有 fmt 的帮助"，而是直接把他引到登录。
    if (topic == "fmt" || topic == "fmt.exe" || topic == "login.exe") {
        std::printf("fmt 是程序本身，不是命令。窗口里登录请敲：login [用户名]\n");
        return true;
    }

    std::fprintf(stderr, "没有 %s 的帮助；输入 help 查看命令列表\n", topic.c_str());
    return false;
}

void print_usage() {
    std::printf("%s\n\n", banner_text().c_str());
    std::printf("用法：fmt.exe [命令]\n");
    std::printf("  不带参数直接双击：创建/检查数据根、确保服务在运行，然后进入交互式命令行。\n");
    std::printf("  登录：fmt.exe -u <用户名> -p     回车后隐藏输入密码（回显 *）\n");
    std::printf("        窗口里也能登录：login [用户名]   （不带用户名就用账号里那个）\n");
    std::printf("        登录态只在本次窗口里；关掉窗口要重新登录。\n");
    std::printf("        业务命令（file / bucket / trash / share / config / user update / user delete）\n");
    std::printf("        需要先登录；service / log / help / version / user get / user create 不需要。\n\n");
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

// ---- service 命令：除 status 外每条都提权（is_user_service_command 定义在文件末尾）----

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

    // 登录态：**只活在本次 CLI 进程的内存里**（用户要求：窗口关掉就要重新登录）。
    // 落盘会让"关掉重开"仍然算登录过，所以这里刻意什么都不存。
    bool logged_in = false;
    std::string username;
    // 登录时算出的口令摘要：`user token renew` 要用它（服务凭摘要换发 token）。
    // 它是摘要而不是明文——同一个窗口里再问一次密码纯属骚扰。
    std::string password_digest;
};

}  // namespace

// user 命令组（`user create|get|update password|delete`）的处理。

// 这两条声明必须放在 `fmt::cli` 层（`Session` 已可见），并且与各自的定义同层：
// 放进匿名名字空间会落到另一个匿名名字空间里（本文件有多个），于是要么链接不上
//（「无法解析的外部符号」），要么变成两份实体（「重载不明确」）——两种都真踩过。
void restart_service_after_config(const Options& options);
int run_user_command(const std::vector<std::string>& parts, Session& session,
                     const Options& options, bool interactive);

// 登录：`fmt -u <用户名> -p`（口令回车后隐藏输入）。
//
// 口令不出本进程：先向服务要 `user.salt`，本地算出摘要再发去比对（op = user.verify）。
// 返回服务确认的用户名；调用方把它记进 Session（**只活在内存里**，窗口一关就没了）。
Result<std::string> verify_login(Session& session, const Options& options,
                                 const std::string& username, const std::string& password,
                                 std::string* digest_out = nullptr);

namespace {

Status ensure_connected(Session& session, const Options& options) {
    if (session.connected) {
        return std::monostate{};
    }

    // 管道名默认是 \\.\pipe\fmt.control；测试进程用 FMT_PIPE 覆盖，
    // 好让端到端测试里的 CLI 打到测试自己的服务、而不是用户的真服务。
    Result<ipc::PipeClient> connected =
        ipc::PipeClient::connect_waiting(kConnectWaitMs, ipc::pipe_name().c_str());
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

    // 「数据根由 CLI 声明」意味着任何位置的 fmt.exe 一连上就会把服务的数据根搬到自己
    // 目录下。以前这件事**只写进日志**，用户在控制台上完全看不到——数据、桶、回收站
    // 会整体换成另一个目录的内容，却没有任何提示。现在在切换发生的那一刻就报出来。
    if (session.root_switched) {
        std::fprintf(stderr, "%s", root_switch_notice(session.previous_root,
                                                      to_forward_slashes(options.data_root))
                                      .c_str());
    }

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
    // 先把 stdout 刷掉再写 stderr：两条流被重定向到同一个管道时（脚本、e2e），
    // stdout 是块缓冲而 stderr 无缓冲，不刷就会出现「错误信息跑到提示符前面」的错觉。
    std::fflush(stdout);
    std::fprintf(stderr, "执行失败：%s %s\n", code_string(error.code).c_str(),
                 error.message.c_str());
    std::fprintf(stderr, "错误码：%d\n", exit_code(error.code));
    std::fflush(stderr);
}

// 定义在下面（print_business_data 与 print_precheck 都要用它）
void print_trash_entry(const nlohmann::json& entry);

// 业务命令的结果按形状打印：Bucket 列表、单条信息、或服务给的 message。
// 服务端返回结构化数据，怎么展示放在 CLI 这一侧。
void print_business_data(const nlohmann::json& data, bool show_token = false) {
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

    // 账号信息（user create / user get / user update 的响应）。
    //
    // 版式与 config list 一致：**文件路径在前**，后面跟字段（一行一个键值对）。
    // 刻意不打印口令哈希、盐与 token —— 那三个是凭证，`user get` 不该顺手吐出来
    // （token 要看走 `config list --show-token`）。
    if (const auto name_field = data.find("username");
        name_field != data.end() && name_field->is_string()) {
        std::printf("账号文件：%s\n", data.value("path", std::string{"data/user.json"}).c_str());
        std::printf("用户名：%s\n", name_field->get<std::string>().c_str());
        if (data.contains("user_id")) {
            std::printf("用户 ID：%s\n", data.value("user_id", std::string{}).c_str());
        }
        if (data.contains("current_bucket")) {
            std::printf("当前 Bucket：%s\n", data.value("current_bucket", std::string{}).c_str());
        }
        if (data.contains("created_at")) {
            std::printf("创建时间：%s\n", data.value("created_at", std::string{}).c_str());
        }
        if (data.contains("updated_at")) {
            std::printf("更新时间：%s\n", data.value("updated_at", std::string{}).c_str());
        }
        if (data.contains("last_login_at")) {
            const std::string last = data.value("last_login_at", std::string{});
            std::printf("最后登录：%s\n", last.empty() ? "（还没登录过）" : last.c_str());
        }
        // 密码还能不能改：这是 `user get` 最该一眼看到的状态。
        if (data.contains("password_changed")) {
            std::printf("密码状态：%s\n",
                        data.value("password_changed", false)
                            ? "已经修改过一次，不能再改（要重设请 user delete 后 user create）"
                            : "还可以修改一次（user update password）");
        }
        // token 的有效期：**不给 token 本身**（要看走 config list --show-token），
        // 但「什么时候过期」必须给——否则用户看到 token 却不知道它还能用多久。
        if (const auto expiry = data.find("token_expiry_text");
            expiry != data.end() && expiry->is_string()) {
            std::printf("token 有效期：%s\n", expiry->get<std::string>().c_str());
        }
        // token 值：`user get` 与 `user token renew` 都会带。
        if (const auto token = data.find("token"); token != data.end() && token->is_string()) {
            std::printf("访问 token：%s\n", token->get<std::string>().c_str());
            if (const auto issued = data.find("token_issued_at");
                issued != data.end() && issued->is_string()) {
                std::printf("token 签发时间：%s\n", issued->get<std::string>().c_str());
            }
            std::printf("请求头：X-FMT-Token: %s\n", token->get<std::string>().c_str());
        }
        // 服务端的提示（例如「密码只能修改一次」）放最后一行。
        if (const auto message = data.find("message");
            message != data.end() && message->is_string()) {
            std::printf("%s\n", message->get<std::string>().c_str());
        }
        return;
    }

    // config.list：config.json 与 config/server.json 的公共字段（本次改动（未提交）
    // 把 server.json 也一并输出）。打成标签行，比在窗口里吐一段 JSON 好读；
    // config.set 没有 current_user，会走 message。
    //
    // 版式约定（都按「同一个后缀」统一风格，用户的四条意见）：
    //   ① 两个文件各自成段：**文件路径在前**，后面跟这个文件的字段；
    //   ② 同一个文件的字段同一套后缀风格：`当前用户 / 当前 Bucket / 上传上限 / 大小单位 /
    //      语言`；网络服务那段是 `enabled / host / port`；
    //   ③ 一行只放一个键值对：大小单位与语言分成两行；
    //   ④ **token 默认不打印**（会暴露在屏幕、日志、工单与截图里）。要看加 `--show-token`，
    //      或者直接读数据根下的 data/user.json。
    if (data.contains("max_upload_size") && data.contains("current_user")) {
        const auto max_bytes = data.value("max_upload_size", std::uintmax_t{0});
        std::printf("配置文件：%s\n", data.value("path", std::string{}).c_str());
        std::printf("当前用户：%s\n", data.value("current_user", std::string{}).c_str());
        std::printf("当前 Bucket：%s\n", data.value("current_bucket", std::string{}).c_str());
        std::printf("上传上限：%s（%zu 字节）\n", format_size(max_bytes).c_str(),
                    static_cast<std::size_t>(max_bytes));
        std::printf("大小单位：%s\n", data.value("size_unit", std::string{}).c_str());
        std::printf("语言：%s\n", data.value("language", std::string{}).c_str());
        // token 的有效期：**下次签发**用时多长，以及当前这个 token 到什么时候失效。
        // 放在 token 之前——先让用户知道"还能用多久"，再看凭证本身。
        if (data.contains("token_ttl_seconds")) {
            const auto ttl = data.value("token_ttl_seconds", std::uint64_t{0});
            std::printf("token 有效期（下次签发）：%llu 秒\n",
                        static_cast<unsigned long long>(ttl));
        }
        if (const auto expiry = data.find("token_expiry_text");
            expiry != data.end() && expiry->is_string()) {
            std::printf("token 当前有效期：%s\n", expiry->get<std::string>().c_str());
        }

        // 网络服务（config/server.json）：文件路径 + 开关 / 监听地址 / 端口。
        // 格式按用户要求：`网络服务 enabled：true`、`服务地址 host：localhost`、
        // `服务端口 port：4122`。
        const auto server_path = data.find("server_path");
        if (server_path != data.end() && server_path->is_string()) {
            std::printf("网络服务配置：%s\n", server_path->get<std::string>().c_str());
        }
        for (const auto& field : {std::pair<const char*, const char*>{"server_enabled", "网络服务 enabled"},
                                  {"server_host", "服务地址 host"},
                                  {"server_port", "服务端口 port"}}) {
            const auto found = data.find(field.first);
            if (found == data.end()) {
                continue;
            }
            std::string shown;
            if (found->is_boolean()) {
                shown = found->get<bool>() ? "true" : "false";
            } else if (found->is_number_integer()) {
                shown = std::to_string(found->get<int>());
            } else if (found->is_string()) {
                shown = found->get<std::string>();
            }
            std::printf("%s：%s\n", field.second, shown.c_str());
        }

        // 用户 ID：不带凭证，照常给。
        if (data.contains("user_id")) {
            std::printf("用户 ID：%s\n", data.value("user_id", std::string{}).c_str());
        }
        // token：默认不说；--show-token 时才打印。
        if (show_token) {
            std::printf("访问 token：%s\n", data.value("token", std::string{}).c_str());
            std::printf("HTTP 请求头：X-FMT-Token: %s\n",
                        data.value("token", std::string{}).c_str());
        } else if (data.contains("token")) {
            std::printf("访问 token：（已隐藏；加 --show-token 显示，或读数据根下的 data/user.json）\n");
        }
        return;
    }

    // 单条分享：share_id 是标志字段（create / get / delete 都返回它）。
    // **必须排在「单条文件信息」之前**：分享数据里也带 file_id 与 size，
    // 否则会被当成文件详情打出来，把分享本身的信息（次数、到期、状态）全丢掉。
    if (data.contains("share_id")) {
        const auto expire = data.find("expire_time");
        const bool expired =
            data.value("expired", false) || data.value("state", std::string{}) == "已过期";
        const bool limit_reached = data.value("limit_reached", false) ||
                                   data.value("state", std::string{}) == "下载次数已用尽";
        std::printf("share_id：%s\n", data.value("share_id", std::string{}).c_str());
        if (data.contains("file_name")) {
            std::printf("文件：%s（file_id %s）\n", data.value("file_name", std::string{}).c_str(),
                        data.value("file_id", std::string{}).c_str());
            if (data.contains("bucket")) {
                std::printf("Bucket：%s\n", data.value("bucket", std::string{}).c_str());
            }
        } else {
            std::printf("file_id：%s\n", data.value("file_id", std::string{}).c_str());
        }
        // 分享链接：完整地址，直接就能粘进浏览器（不看 HTTP 服务有没有开）。
        // `share get` 要「看全部信息」，所以链接**照给**——但当前不可下载（过期 / 次数用尽 /
        // 已撤销 / 文件进回收站）时补一句，免得用户拿去试了才发现打不开。
        if (data.contains("share_url")) {
            const bool usable = data.value("available", false);
            std::printf("分享链接：%s%s\n", data.value("share_url", std::string{}).c_str(),
                        usable ? "" : "（当前不可下载，链接已失效）");
        }
        // 下载次数：用满时把话说明白（用户要的就是这一句「下载次数已满」）
        std::printf("下载次数：%d/%d%s\n", data.value("download_count", 0),
                    data.value("max_download_count", 0), limit_reached ? "（下载次数已满）" : "");
        // 到期：过期时同样说明白；注意「已过期」与「次数已满」是两个独立维度，
        // 可能同时成立，所以两行各报各的。
        const std::string expire_text = (expire != data.end() && expire->is_string())
                                            ? expire->get<std::string>()
                                            : std::string{};
        if (expire_text.empty()) {
            std::printf("到期：不过期%s\n", expired ? "（已过期）" : "");
        } else {
            std::printf("到期：%s%s\n", expire_text.c_str(), expired ? "（已过期）" : "");
        }
        if (data.contains("state")) {
            std::printf("状态：%s\n", data.value("state", std::string{}).c_str());
        }
        if (data.contains("message")) {
            std::printf("%s\n", data.value("message", std::string{}).c_str());
        }
        return;
    }

    // 分享列表：shares: [{share_id, file_name, download_count, state, share_url, …}, …]
    // 每行一条，**把「下载次数已满」「已过期」直接标在行的末尾**（用户要求）：
    //   0d7b3cf4bc2e  report.txt  20/20  下载次数已满  到期 2026-10-20T14:03:11
    if (const auto items = data.find("shares"); items != data.end() && items->is_array()) {
        const std::string current_bucket = data.value("current_bucket", std::string{});
        for (const nlohmann::json& item : *items) {
            const auto expire = item.find("expire_time");
            const std::string state = item.value("state", std::string{});
            const std::string name = item.value("file_name", std::string{});
            const bool in_current = !current_bucket.empty() &&
                                    item.value("bucket", std::string{}) == current_bucket;
            std::printf("  %s  %s%s  %d/%d  %s", item.value("share_id", std::string{}).c_str(),
                        name.empty() ? item.value("file_id", std::string{}).c_str() : name.c_str(),
                        in_current ? "（当前桶）" : "", item.value("download_count", 0),
                        item.value("max_download_count", 0), state.c_str());
            // 两个「不能下载」的原因**独立标注**：可以既过期、次数又用满。
            std::string marks;
            if (item.value("expired", false) || state == "已过期") {
                marks += "下载已过期";
            }
            if (item.value("limit_reached", false) || state == "下载次数已用尽") {
                marks += marks.empty() ? "下载次数已满" : "、下载次数已满";
            }
            if (!marks.empty()) {
                std::printf("  [%s]", marks.c_str());
            }
            std::printf("  到期 %s\n", (expire != item.end() && expire->is_string())
                                           ? expire->get<std::string>().c_str()
                                           : "不过期");
            // **打印链接**这件事在这里是有意不做的（用户要求：列表只要一行状态）。
            // 链接仍然在 data 里（`share_url`），要看单条用 `share get <share_id>`
            // ——列表一屏能看很多条，每条再挂一行链接会把状态淹掉。
        }
        if (data.value("scope", std::string{}) == "all") {
            std::printf("共 %zu 条分享（全部文件，最近创建的在前）\n", items->size());
        } else {
            std::printf("共 %zu 条分享（file_id %s）\n", items->size(),
                        data.value("file_id", std::string{}).c_str());
        }
        return;
    }

    // 文件列表：files: [{file_id, file_name, size}, …]
    // 有 total 就用它（分页时 items->size() 只是本页条数，说"共 N"会骗人）。
    if (const auto items = data.find("files"); items != data.end() && items->is_array()) {
        for (const nlohmann::json& item : *items) {
            std::printf("  %s  %s\n", item.value("file_name", std::string{}).c_str(),
                        format_size(item.value("size", std::uintmax_t{0})).c_str());
        }
        const std::size_t total = data.value("total", items->size());
        if (data.value("search", std::string{}).empty()) {
            std::printf("共 %zu 个文件", total);
        } else {
            std::printf("匹配「%s」共 %zu 个文件", data.value("search", std::string{}).c_str(),
                        total);
        }
        const int page_size = data.value("page_size", 0);
        if (page_size > 0) {
            std::printf("（第 %d/%d 页，本页 %zu 条）", data.value("page", 1),
                        data.value("total_pages", 0), items->size());
        }
        std::printf("\n");
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
        // 预览 / 下载链接（本次改动（未提交））：完整地址，直接粘进浏览器即可，
        // **不看 HTTP 服务有没有开**（地址来自 config/server.json）。
        // 预览只对图片与文本类有效（其余类型浏览器会拿到 FMT-701），下载任何类型都行。
        if (data.contains("preview_url")) {
            std::printf("预览链接：%s\n", data.value("preview_url", std::string{}).c_str());
        }
        if (data.contains("download_url")) {
            std::printf("下载链接：%s\n", data.value("download_url", std::string{}).c_str());
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

    // 合并回退的同名冲突：把撞上的名字摊开，用户才知道该给谁改名
    if (const auto conflicts = data.find("conflicts");
        conflicts != data.end() && conflicts->is_array()) {
        std::printf("同名冲突：\n");
        for (const nlohmann::json& item : *conflicts) {
            std::printf("  %s\n", item.get<std::string>().c_str());
        }
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

}  // namespace

// user 组：账号的创建 / 查看 / 改密 / 删除。
//
// 三条贯穿始终的规矩：
//   ① **口令明文不出本进程**：创建时先算摘要再发送；改密走挑战-应答（服务给 nonce，
//      这里算应答值），所以 `data/user.json` 的口令永远不经过命名管道，也不落任何日志。
//   ② **命令名与其余命令组一致**：`user create` / `user get` / `user update password` /
//      `user delete`，实际走管道的是 `user.create` / `user.get` / `user.update` / `user.delete`。
//   ③ **一个数据根一个账号**：已有账号一律拒绝重建，要重建先 `user delete`。

// 永久删除数据根：`user delete` 的最后一步。
//
// 口径（用户明确要求）：
//   * **真删**：不走回收站、不可撤销，连桶与回收站目录一起删；
//   * **只留 fmt.exe**：exe 与 log/ 之外的目录/文件全部删除（log/ 留着是因为
//     它正被本进程写着，删了会得到"删不干净"的假失败）；
//   * 要求用户逐字输入 `delete` 才动手——密码在上一层已经验过了。
int delete_data_root(const Options& options) {
    const std::filesystem::path root = std::filesystem::path(fmt::path_from_utf8(options.data_root));
    const PathManager paths(root);

    std::vector<std::filesystem::path> targets = {
        paths.repository(), paths.trash(), paths.config(),
        paths.data(),       paths.temp(),
    };

    std::printf("\n即将**永久删除**这个数据根下的全部数据：\n");
    for (const std::filesystem::path& item : targets) {
        std::printf("  %s\n", fmt::path_to_utf8(item).c_str());
    }
    std::printf("以及 %s 与 %s。\n", fmt::path_to_utf8(paths.user_data()).c_str(),
                fmt::path_to_utf8(paths.file_data()).c_str());
    std::printf("只保留 fmt.exe 与 log/。\n");
    std::printf("**不进回收站、无法撤销**：账号、令牌、桶里的文件、回收站里的历史都会消失。\n");
    std::printf("删完之后目录结构会自动重建，可以立刻 `user create <用户名>` 建新账号。\n");
    std::printf("确认请逐字输入 delete（其它任何输入都会取消）：");
    std::fflush(stdout);

    std::string answer;
    if (!std::getline(std::cin, answer)) {
        std::printf("\n");
        return exit_code(ErrorCode::InvalidArgument);
    }
    if (trim(answer) != "delete") {
        std::printf("已取消（没有删除任何东西）。\n");
        return 0;
    }

    int failures = 0;
    for (const std::filesystem::path& item : targets) {
        std::error_code ignored;
        if (!std::filesystem::exists(item, ignored)) {
            continue;
        }
        std::filesystem::remove_all(item, ignored);
        if (ignored) {
            std::fprintf(stderr, "删除失败：%s（%s）\n", fmt::path_to_utf8(item).c_str(),
                         ignored.message().c_str());
            ++failures;
        } else {
            std::printf("已删除 %s\n", fmt::path_to_utf8(item).c_str());
        }
    }

    log_warn("Cli", "user delete：数据根 " + options.data_root + " 的业务数据已永久删除");
    if (failures > 0) {
        std::fprintf(stderr,
                     "\n有 %d 项没能删除。若提示文件被占用，请先 `service stop` 再重试。\n",
                     failures);
        return exit_code(ErrorCode::StorageError);
    }

    // 删完立刻把目录骨架建回来（目录 + 四个默认 JSON，**不建账号**）。
    //
    // 为什么必须重建：`user create` 有「关键数据文件缺失就拒绝」的保护，
    // 而刚删掉 data/ 之后四个文件都不在——不重建的话用户还得关掉窗口重开一次
    // 才能建账号（真机流程里那一步很突兀）。
    const Result<RootRepair> rebuilt = ensure_root(paths);
    if (!ok(rebuilt)) {
        std::fprintf(stderr, "数据已删除，但目录结构重建失败：%s\n",
                     error_of(rebuilt)->message.c_str());
        return exit_code(ErrorCode::StorageError);
    }

    // ensure_root 写的是默认 config；再显式保证当前桶为空，避免旧配置残留。
    if (Result<Config> loaded = load_config(paths); ok(loaded)) {
        Config fresh = std::get<Config>(loaded);
        if (!fresh.current_bucket.empty()) {
            fresh.current_bucket.clear();
            if (const Status saved = save_config(paths, fresh); !ok(saved)) {
                std::fprintf(stderr, "清空当前 Bucket 失败：%s\n",
                             error_of(saved)->message.c_str());
            }
        }
    }

    // 服务可能还在跑并持有旧账号/旧桶的内存副本；删完通知它重新读盘。
    if (service::query_state() == service::State::Running) {
        Session session;
        if (const Status connected = ensure_connected(session, options); ok(connected)) {
            ipc::Request reload;
            reload.id = session.next_id++;
            reload.op = "service.reload";
            reload.root = options.data_root;
            reload.pid = GetCurrentProcessId();
            Result<ipc::Response> response =
                session.client.call(reload, ipc::kCommandTimeoutMs);
            if (!ok(response) || !std::get<ipc::Response>(response).ok) {
                const std::string reason =
                    !ok(response) ? error_of(response)->message
                                  : std::get<ipc::Response>(response).error.message;
                std::fprintf(stderr,
                             "服务未能自动重载（%s）；请执行 service reinstall 或重启服务。\n",
                             reason.c_str());
            } else {
                std::printf("服务已重新加载：当前用户/当前 Bucket 已清空。\n");
            }
        }
    }

    std::printf("\n数据已永久删除，目录结构已重建。\n");
    std::printf("现在可以直接建新账号：user create <用户名>\n");
    return 0;
}

// 让用户把口令打两遍。返回 false = 两次不一致（或输入被中断），调用方要放弃。
bool ask_password_twice(const char* what, std::string* out) {    std::string first;
    std::string second;
    if (!read_password_masked(("请输入" + std::string(what) + "：").c_str(), &first)) {
        return false;
    }
    if (first.empty()) {
        std::fprintf(stderr, "口令不能为空\n");
        return false;
    }
    if (!read_password_masked(("请再输入一次" + std::string(what) + "：").c_str(), &second)) {
        return false;
    }
    if (first != second) {
        std::fprintf(stderr, "两次输入的口令不一致，本次操作已放弃\n");
        return false;
    }
    *out = first;
    return true;
}

// 登录：`fmt -u <用户名> -p`。
//
// 三步：要口令盐（user.salt）→ 本地算摘要 → 发去比对（user.verify）。
// **口令明文不出本进程**；登录态由调用方写进 Session 的内存字段。
Result<std::string> verify_login(Session& session, const Options& options,
                                 const std::string& username, const std::string& password,
                                 std::string* digest_out) {
    if (username.empty()) {
        return make_error(ErrorCode::InvalidArgument, "请给出用户名：fmt -u <用户名> -p");
    }
    if (password.empty()) {
        return make_error(ErrorCode::InvalidArgument, "口令不能为空");
    }

    ipc::Request request;
    request.root = options.data_root;
    request.pid = GetCurrentProcessId();

    // ① 要盐
    request.op = "user.salt";
    request.id = session.next_id++;
    Result<ipc::Response> salt_response = session.client.call(request, ipc::kCommandTimeoutMs);
    if (!ok(salt_response)) {
        return *error_of(salt_response);
    }
    if (!std::get<ipc::Response>(salt_response).ok) {
        return std::get<ipc::Response>(salt_response).error;
    }
    const std::string salt =
        std::get<ipc::Response>(salt_response).data.value("salt", std::string{});
    if (salt.empty()) {
        return make_error(ErrorCode::StorageError, "服务没有给出账号口令盐，无法登录");
    }

    // ② 本地算摘要（明文到此为止，之后只发摘要）
    const std::string digest = fmt::password_digest(password, salt);
    if (digest.empty()) {
        return make_error(ErrorCode::StorageError, "无法计算口令摘要（CSPRNG/PBKDF2 失败）");
    }
    if (digest_out != nullptr) {
        *digest_out = digest;  // 供同一窗口内的 user token renew 复用，免得再问一次密码
    }

    // ③ 比对
    request.op = "user.verify";
    request.id = session.next_id++;
    request.args = nlohmann::json::object();
    request.args["current_digest"] = digest;
    Result<ipc::Response> verified = session.client.call(request, ipc::kCommandTimeoutMs);
    if (!ok(verified)) {
        return *error_of(verified);
    }
    if (!std::get<ipc::Response>(verified).ok) {
        return std::get<ipc::Response>(verified).error;
    }
    return std::get<ipc::Response>(verified).data.value("username", username);
}

// 门禁提示里要写的「命令标签」：`file list`、`user delete` 这样给到二级，
// 而不是只说 `user` —— 否则用户知道被拦了、却不知道是哪条被拦。
std::string command_label(const std::vector<std::string>& parts) {
    if (parts.empty()) {
        return {};
    }
    // `user` 组要显示到三级（`user token renew`、`user update password`）：
    // 只说「user token」用户还是不知道该敲哪条。
    const std::size_t want = (parts[0] == "user") ? 3 : 2;
    std::string label = parts[0];
    for (std::size_t i = 1; i < parts.size() && i < want; ++i) {
        label += " " + parts[i];
    }
    return label;
}

// 门禁：没登录就不放行业务命令（用户要求）。
//
// `service *` / `log` / `help` / `version` / `exit` 一律放行——它们与业务数据无关，
// 而且服务坏了、账号建不出来的时候，还得靠它们把机器救回来。
bool business_allowed(const std::string& head, bool account_exists) {
    if (head == "service" || head == "log" || head == "help" || head == "version" ||
        head == "exit" || head == "quit" || head == "login" || head == "logout" || head == "fmt") {
        return true;
    }
    // 还没有账号：放行 user 组（得先能建账号）。
    if (!account_exists) {
        return head == "user";
    }
    return false;
}

int run_user_command(const std::vector<std::string>& parts, Session& session,
                     const Options& options, bool interactive) {
    if (parts.size() < 2) {
        std::fprintf(stderr,
                     "用法：user create <用户名> | user get | user update password | user delete\n");
        return exit_code(ErrorCode::InvalidArgument);
    }
    const std::string& verb = parts[1];

    // **`user delete` 在连接服务之前处理**：它不需要登录、也不需要密码。
    //
    // 用户的原话：「用户不记得密码 还不能重新创建用户吗？user delete 为什么要登录？」
    // 判断成立——删号是「忘记密码」时唯一的逃生门。若这里还要密码，门就锁死了
    // （密码记不住，还得用它来删号，等于永远进不去）。
    //
    // 安全口径：这个数据根的所有者是本机这个 Windows 用户，他本来就能用资源管理器
    // 直接删 data/；密码校验在这里只是仪式，不是边界。真正的防误触是「逐字输入 delete」。
    // 也正因为不碰服务，服务没装/没跑的时候照样能删。
    if (verb == "delete") {
        return delete_data_root(options);
    }

    if (const Status status = ensure_connected(session, options); !ok(status)) {
        log_error("Cli", "user 命令无法连接服务：" + error_of(status)->message);
        print_failure(*error_of(status));
        return exit_code(error_of(status)->code);
    }

    ipc::Request request;
    request.root = options.data_root;
    request.pid = GetCurrentProcessId();
    request.args = nlohmann::json::object();

    if (verb == "create") {
        if (parts.size() < 3) {
            std::fprintf(stderr, "用法：user create <用户名>\n");
            return exit_code(ErrorCode::InvalidArgument);
        }
        const std::string username = parts[2];

        // **先查有没有账号，再问口令**（真机反馈：以前是先让用户输入两次密码，
        // 最后才报「已经有账号了」——白输一遍）。这里问服务要账号信息：
        // 没有账号时它会明确回 FMT-002，那种失败是"预期状态"，直接放行继续建。
        {
            ipc::Request who = request;
            who.op = "user.get";
            who.id = session.next_id++;
            Result<ipc::Response> existing = session.client.call(who, ipc::kCommandTimeoutMs);
            if (!ok(existing)) {
                session.connected = false;
                print_failure(*error_of(existing));
                return exit_code(error_of(existing)->code);
            }
            const ipc::Response& found = std::get<ipc::Response>(existing);
            if (found.ok) {
                const std::string name = found.data.value("username", std::string{"?"});
                std::fprintf(stderr,
                             "这个数据根已经有账号了（%s）。一个数据根只能有一个账号。\n"
                             "要换账号或重设密码，先删除用户再重建：\n"
                             "  user delete          （需要密码；再逐字输入 delete 才真删，"
                             "不可撤销）\n"
                             "  user create <用户名>\n"
                             "只想改密码（只能改一次）：user update password\n",
                             name.c_str());
                log_warn("Cli", "user create 被拒：数据根已有账号 " + name);
                return exit_code(ErrorCode::InvalidArgument);
            }
            // 没有账号（FMT-002）→ 正常继续；其它错误如实透出。
            if (found.error.code != ErrorCode::FileNotFound) {
                print_failure(found.error);
                return exit_code(found.error.code);
            }
        }

        std::string password;
        if (!ask_password_twice("密码", &password)) {
            return exit_code(ErrorCode::InvalidArgument);
        }
        const std::string digest = fmt::password_digest(password, fmt::directory_salt(username));
        password.assign(password.size(), '\0');
        if (digest.empty()) {
            std::fprintf(stderr, "无法计算口令摘要（CSPRNG/PBKDF2 失败）\n");
            return exit_code(ErrorCode::StorageError);
        }
        request.op = "user.create";
        request.args["username"] = username;
        request.args["password_digest"] = digest;  // 只有摘要过管道，没有明文
    } else if (verb == "get") {
        request.op = "user.get";
    } else if (verb == "token") {
        // `user token renew`：换发访问 token。**token 有时效**（用户要求「过时不能用」），
        // 过期后这是唯一的续期路径——凭密码，不凭旧 token（旧 token 已经不能用了）。
        if (parts.size() < 3 || parts[2] != "renew") {
            std::fprintf(stderr, "用法：user token renew\n");
            return exit_code(ErrorCode::InvalidArgument);
        }

        // 摘要优先用登录时算好的那一份（同一个窗口里不用再问一遍密码）；
        // 没登录过就当场问一次。明文始终不出本进程。
        std::string digest = session.password_digest;
        if (digest.empty()) {
            std::string username;
            std::string salt;
            {
                ipc::Request who = request;
                who.op = "user.get";
                who.id = session.next_id++;
                Result<ipc::Response> named = session.client.call(who, ipc::kCommandTimeoutMs);
                if (!ok(named)) {
                    session.connected = false;
                    print_failure(*error_of(named));
                    return exit_code(error_of(named)->code);
                }
                if (!std::get<ipc::Response>(named).ok) {
                    print_failure(std::get<ipc::Response>(named).error);
                    return exit_code(std::get<ipc::Response>(named).error.code);
                }
                username = std::get<ipc::Response>(named).data.value("username", std::string{});
            }
            {
                ipc::Request want = request;
                want.op = "user.salt";
                want.id = session.next_id++;
                Result<ipc::Response> got = session.client.call(want, ipc::kCommandTimeoutMs);
                if (!ok(got)) {
                    session.connected = false;
                    print_failure(*error_of(got));
                    return exit_code(error_of(got)->code);
                }
                if (!std::get<ipc::Response>(got).ok) {
                    print_failure(std::get<ipc::Response>(got).error);
                    return exit_code(std::get<ipc::Response>(got).error.code);
                }
                salt = std::get<ipc::Response>(got).data.value("salt", std::string{});
            }
            std::string password;
            if (!read_password_masked("请输入密码：", &password)) {
                return exit_code(ErrorCode::InvalidArgument);
            }
            digest = fmt::password_digest(password, salt);
            password.assign(password.size(), '\0');
            if (digest.empty()) {
                std::fprintf(stderr, "无法计算口令摘要（CSPRNG/PBKDF2 失败）\n");
                return exit_code(ErrorCode::StorageError);
            }
            session.logged_in = true;
            session.username = username;
            session.password_digest = digest;
        }

        request.op = "user.token.renew";
        request.args["current_digest"] = digest;
    } else if (verb == "update") {
        // update 目前只有 password 一项（用户的原话是「user update password」），
        // 命令里仍然显式写出来：以后加别的可改项时，这条语法不用改。
        if (parts.size() < 3 || parts[2] != "password") {
            std::fprintf(stderr, "用法：user update password\n");
            return exit_code(ErrorCode::InvalidArgument);
        }

        // 口令盐与用户名都先问服务要：账号由数据根决定，CLI 不猜。
        // 盐必须用服务给的那一份——**老账号是随机盐**，CLI 自己派生会算错摘要，
        // 把老账号的口令变成验不过。
        // 口令明文不出本进程：旧口令当场派生成摘要，新口令也一样——过管道的只有摘要。
        std::string username;
        std::string salt;
        {
            ipc::Request who = request;
            who.op = "user.get";
            who.id = session.next_id++;
            Result<ipc::Response> named = session.client.call(who, ipc::kCommandTimeoutMs);
            if (!ok(named)) {
                session.connected = false;
                print_failure(*error_of(named));
                return exit_code(error_of(named)->code);
            }
            if (!std::get<ipc::Response>(named).ok) {
                print_failure(std::get<ipc::Response>(named).error);
                return exit_code(std::get<ipc::Response>(named).error.code);
            }
            username = std::get<ipc::Response>(named).data.value("username", std::string{});
        }
        {
            ipc::Request want = request;
            want.op = "user.salt";
            want.id = session.next_id++;
            Result<ipc::Response> got = session.client.call(want, ipc::kCommandTimeoutMs);
            if (!ok(got)) {
                session.connected = false;
                print_failure(*error_of(got));
                return exit_code(error_of(got)->code);
            }
            if (!std::get<ipc::Response>(got).ok) {
                print_failure(std::get<ipc::Response>(got).error);
                return exit_code(std::get<ipc::Response>(got).error.code);
            }
            salt = std::get<ipc::Response>(got).data.value("salt", std::string{});
        }
        if (salt.empty()) {
            std::fprintf(stderr, "服务没有给出账号口令盐，无法校验\n");
            return exit_code(ErrorCode::StorageError);
        }

        std::string current;
        if (!read_password_masked("请输入当前密码：", &current)) {
            return exit_code(ErrorCode::InvalidArgument);
        }
        const std::string current_digest = fmt::password_digest(current, salt);
        current.assign(current.size(), '\0');  // 用完立刻抹掉本进程里的明文

        std::string fresh;
        if (!ask_password_twice("新密码", &fresh)) {
            return exit_code(ErrorCode::InvalidArgument);
        }
        const std::string digest = fmt::password_digest(fresh, salt);
        fresh.assign(fresh.size(), '\0');
        if (current_digest.empty() || digest.empty()) {
            std::fprintf(stderr, "无法计算口令摘要（CSPRNG/PBKDF2 失败）\n");
            return exit_code(ErrorCode::StorageError);
        }

        request.op = "user.update";
        request.args["current_digest"] = current_digest;  // 旧口令的摘要：服务只做比对
        request.args["password_digest"] = digest;         // 新口令的摘要
    } else {
        std::fprintf(stderr, "未知的 user 子命令：%s\n\n", verb.c_str());
        return exit_code(ErrorCode::InvalidArgument);
    }

    request.id = session.next_id++;
    Result<ipc::Response> response = session.client.call(request, ipc::kCommandTimeoutMs);
    if (!ok(response)) {
        session.connected = false;
        print_failure(*error_of(response));
        return exit_code(error_of(response)->code);
    }
    const ipc::Response& value = std::get<ipc::Response>(response);
    if (!value.ok) {
        print_failure(value.error);
        return exit_code(value.error.code);
    }
    print_business_data(value.data, /*show_token=*/false);
    return 0;
}

// ---- upload：大文件分片上传（CLI-1） ----
//
// 服务端 upload.start 起后台线程，所以这里不需要自己逐片发送，也不会长期占着
// 单管道连接；pause/status/cancel 还能在同一个窗口继续发。
Result<ipc::Response> upload_call(Session& session, const Options& options,
                                  const std::string& operation, nlohmann::json args) {
    if (const Status connected = ensure_connected(session, options); !ok(connected)) {
        return *error_of(connected);
    }
    ipc::Request request;
    request.id = session.next_id++;
    request.op = operation;
    request.root = options.data_root;
    request.pid = GetCurrentProcessId();
    request.args = std::move(args);
    return session.client.call(request, ipc::kCommandTimeoutMs);
}

bool stdout_is_console() {
    DWORD mode = 0;
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    return output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode) != 0;
}

std::string upload_state_text(const std::string& state) {
    if (state == "uploading") return "上传中";
    if (state == "paused") return "已暂停";
    if (state == "completing") return "正在完成";
    if (state == "completed") return "已完成";
    if (state == "canceled") return "已取消";
    if (state == "error") return "失败";
    return state;
}

struct UploadProgressView {
    bool console = stdout_is_console();
    bool started = false;
    std::uint64_t last_bytes = 0;
    std::chrono::steady_clock::time_point last_time{};
    double speed = 0.0;
    int last_percent = -1;
    std::string last_state;
    std::size_t last_width = 0;
    int spinner = 0;
};

std::string upload_eta(std::uint64_t remaining, double speed) {
    if (remaining == 0) {
        return "00:00";
    }
    if (speed <= 0.0) {
        return "--:--";
    }
    std::uint64_t seconds = static_cast<std::uint64_t>(remaining / speed);
    if (seconds > 99ULL * 60 * 60) {
        return ">99h";
    }
    std::ostringstream text;
    text << std::setw(2) << std::setfill('0') << (seconds / 3600) << ':' << std::setw(2)
         << ((seconds / 60) % 60) << ':' << std::setw(2) << (seconds % 60);
    return text.str();
}

void render_upload_progress(const nlohmann::json& data, UploadProgressView& view,
                            bool force = false) {
    const std::uint64_t size = data.value("size", std::uint64_t{0});
    const std::uint64_t uploaded = data.value("uploaded_bytes", std::uint64_t{0});
    int percent = data.value("percent", 0);
    percent = std::max(0, std::min(100, percent));
    const std::string state = data.value("state", std::string{});
    const std::string label = upload_state_text(state);

    const auto now = std::chrono::steady_clock::now();
    if (view.started) {
        const double seconds =
            std::chrono::duration<double>(now - view.last_time).count();
        if (seconds >= 0.15) {
            if (uploaded >= view.last_bytes) {
                const double instant =
                    static_cast<double>(uploaded - view.last_bytes) / seconds;
                view.speed = view.speed <= 0.0 ? instant
                                               : view.speed * 0.7 + instant * 0.3;
            } else {
                // 坏分片被删除时进度会回退，速度重新从 0 估。
                view.speed = 0.0;
            }
            view.last_bytes = uploaded;
            view.last_time = now;
        }
    } else {
        view.started = true;
        view.last_bytes = uploaded;
        view.last_time = now;
    }

    const bool hashing = state == "uploading" && uploaded == 0 &&
                         data.value("md5", std::string{}).empty();
    std::ostringstream line;
    if (hashing) {
        static const char kSpinner[] = "|/-\\";
        line << "正在校验源文件 MD5，请稍候… " << kSpinner[view.spinner++ % 4];
    } else {
        const int width = 28;
        const int filled =
            size == 0 ? (state == "completed" ? width : 0)
                      : static_cast<int>((uploaded * width) / size);
        std::string bar(static_cast<std::size_t>(std::max(0, std::min(width, filled))), '#');
        bar.append(static_cast<std::size_t>(width - static_cast<int>(bar.size())), '-');
        const double remaining =
            static_cast<double>(size > uploaded ? size - uploaded : 0);
        const std::string speed_text =
            state == "uploading" && view.speed > 0.0
                ? format_size(static_cast<std::uintmax_t>(view.speed)) + "/s"
                : "--";
        line << '[' << bar << "] " << std::setw(3) << percent << "%  "
             << format_size(uploaded) << " / " << format_size(size) << "  " << speed_text
             << "  ETA " << upload_eta(static_cast<std::uint64_t>(remaining),
                                       state == "uploading" ? view.speed : 0.0)
             << "  " << label;
        if (data.value("chunk_count", std::uint64_t{0}) > 0) {
            line << "  chunk " << data.value("received_count", std::uint64_t{0}) << '/'
                 << data.value("chunk_count", std::uint64_t{0});
        }
    }

    const std::string text = line.str();
    if (view.console) {
        std::string padded = text;
        if (padded.size() < view.last_width) {
            padded.append(view.last_width - padded.size(), ' ');
        }
        std::printf("\r%s", padded.c_str());
        std::fflush(stdout);
        view.last_width = padded.size();
    } else if (force || state != view.last_state || percent / 5 != view.last_percent / 5 ||
               view.last_percent < 0) {
        std::printf("%s\n", text.c_str());
        std::fflush(stdout);
    }
    view.last_state = state;
    view.last_percent = percent;
}

void print_upload_one(const nlohmann::json& data) {
    std::printf("upload_id = %s\n", data.value("upload_id", std::string{}).c_str());
    std::printf("file      = %s\n", data.value("file_name", std::string{}).c_str());
    std::printf("size      = %s\n",
                format_size(data.value("size", std::uintmax_t{0})).c_str());
    std::printf("chunk     = %s × %llu\n",
                format_size(data.value("chunk_size", std::uintmax_t{0})).c_str(),
                static_cast<unsigned long long>(data.value("chunk_count", std::uint64_t{0})));
    std::printf("state     = %s\n", data.value("state", std::string{}).c_str());
    std::printf("progress  = %s / %s（%d%%）\n",
                format_size(data.value("uploaded_bytes", std::uintmax_t{0})).c_str(),
                format_size(data.value("size", std::uintmax_t{0})).c_str(),
                data.value("percent", 0));
    std::printf("chunks    = %llu / %llu\n",
                static_cast<unsigned long long>(data.value("received_count", std::uint64_t{0})),
                static_cast<unsigned long long>(data.value("chunk_count", std::uint64_t{0})));
    if (data.contains("verified_count")) {
        std::printf("verified  = %llu（分片 MD5 已记录）\n",
                    static_cast<unsigned long long>(
                        data.value("verified_count", std::uint64_t{0})));
    }
    if (data.contains("expires_at")) {
        std::printf("expires   = %s\n", data.value("expires_at", std::string{}).c_str());
    }
    if (data.value("reused", false)) {
        std::printf("reused    = %s\n", data.value("message", std::string{}).c_str());
    }
    if (!data.value("final_file_id", std::string{}).empty()) {
        std::printf("file_id   = %s\n", data.value("final_file_id", std::string{}).c_str());
    }
    if (!data.value("error", std::string{}).empty()) {
        std::printf("error     = %s\n", data.value("error", std::string{}).c_str());
    }
}

int run_upload_command(const std::vector<std::string>& parts, Session& session,
                       const Options& options) {
    const std::string usage =
        "用法：upload start <本地文件> [文件名] [--chunk-size 8MB] [--wait]\n"
        "      upload status|pause|resume|cancel <upload_id>\n"
        "      upload list | upload wait <upload_id>";
    if (parts.size() < 2) {
        std::fprintf(stderr, "%s\n", usage.c_str());
        return exit_code(ErrorCode::InvalidArgument);
    }

    const std::string& verb = parts[1];
    nlohmann::json args = nlohmann::json::object();
    args["argv"] = nlohmann::json::array();

    if (verb == "start") {
        std::string path;
        std::string name;
        std::uintmax_t chunk_size = 0;
        bool wait = false;
        for (std::size_t i = 2; i < parts.size(); ++i) {
            if (parts[i] == "--wait") {
                wait = true;
                continue;
            }
            if (parts[i] == "--chunk-size" && i + 1 < parts.size()) {
                if (!parse_log_budget(parts[++i], &chunk_size)) {
                    std::fprintf(stderr, "无效的 --chunk-size：%s\n%s\n", parts[i].c_str(), usage.c_str());
                    return exit_code(ErrorCode::InvalidArgument);
                }
                continue;
            }
            if (path.empty()) {
                path = parts[i];
            } else if (name.empty()) {
                name = parts[i];
            } else {
                std::fprintf(stderr, "%s\n", usage.c_str());
                return exit_code(ErrorCode::InvalidArgument);
            }
        }
        if (path.empty()) {
            std::fprintf(stderr, "%s\n", usage.c_str());
            return exit_code(ErrorCode::InvalidArgument);
        }

        // 50MB 以下保持旧的单请求路径；只有大文件才建分片会话。
        std::error_code size_error;
        const std::uintmax_t file_size = std::filesystem::file_size(path_from_utf8(path), size_error);
        if (!size_error) {
            Result<ipc::Response> config_response =
                upload_call(session, options, "config.list", nlohmann::json::object());
            if (ok(config_response)) {
                const ipc::Response& config = std::get<ipc::Response>(config_response);
                const std::uint64_t max_upload =
                    config.ok ? config.data.value("max_upload_size", std::uint64_t{0}) : 0;
                if (config.ok && max_upload > 0 && file_size <= max_upload) {
                    if (const Status connected = ensure_connected(session, options); !ok(connected)) {
                        print_failure(*error_of(connected));
                        return exit_code(error_of(connected)->code);
                    }
                    ipc::Request request;
                    request.id = session.next_id++;
                    request.op = "file.upload";
                    request.root = options.data_root;
                    request.pid = GetCurrentProcessId();
                    request.args["argv"] = nlohmann::json::array({path});
                    if (!name.empty()) {
                        request.args["argv"].push_back(name);
                    }
                    Result<ipc::Response> response =
                        session.client.call(request, ipc::kUploadTimeoutMs);
                    if (!ok(response)) {
                        print_failure(*error_of(response));
                        return exit_code(error_of(response)->code);
                    }
                    const ipc::Response& value = std::get<ipc::Response>(response);
                    if (!value.ok) {
                        print_failure(value.error);
                        return exit_code(value.error.code);
                    }
                    print_business_data(value.data);
                    return 0;
                }
            }
        }

        args["argv"].push_back(path);
        if (!name.empty()) {
            args["argv"].push_back(name);
        }
        if (chunk_size > 0) {
            args["chunk_size"] = chunk_size;
        }

        Result<ipc::Response> response = upload_call(session, options, "upload.start", args);
        if (!ok(response)) {
            print_failure(*error_of(response));
            return exit_code(error_of(response)->code);
        }
        const ipc::Response& value = std::get<ipc::Response>(response);
        if (!value.ok) {
            print_failure(value.error);
            return exit_code(value.error.code);
        }
        print_upload_one(value.data);
        if (!wait) {
            if (value.data.value("state", std::string{}) == "completed") {
                std::printf("秒传完成：%s（%s）\n",
                            value.data.value("final_file_id", std::string{}).c_str(),
                            value.data.value("file_name", std::string{}).c_str());
                return 0;
            }
            std::printf("后台上传中：upload status %s / upload pause %s\n",
                        value.data.value("upload_id", std::string{}).c_str(),
                        value.data.value("upload_id", std::string{}).c_str());
            return 0;
        }
        // --wait：转成 upload wait，复用同一套轮询输出。
        const std::string upload_id = value.data.value("upload_id", std::string{});
        return run_upload_command({"upload", "wait", upload_id}, session, options);
    }

    std::string id;
    if (verb == "status" || verb == "pause" || verb == "resume" || verb == "cancel" ||
        verb == "wait") {
        if (parts.size() < 3 || parts[2].empty()) {
            std::fprintf(stderr, "%s\n", usage.c_str());
            return exit_code(ErrorCode::InvalidArgument);
        }
        id = parts[2];
    }

    if (verb == "list") {
        for (std::size_t i = 2; i < parts.size(); ++i) {
            if (parts[i] == "--all") {
                args["all"] = true;
            }
        }
        Result<ipc::Response> response = upload_call(session, options, "upload.list", args);
        if (!ok(response)) {
            print_failure(*error_of(response));
            return exit_code(error_of(response)->code);
        }
        const ipc::Response& value = std::get<ipc::Response>(response);
        if (!value.ok) {
            print_failure(value.error);
            return exit_code(value.error.code);
        }
        const auto uploads = value.data.find("uploads");
        if (uploads != value.data.end() && uploads->is_array()) {
            for (const auto& item : *uploads) {
                std::printf("  %s  %s  %d%%  %s", item.value("upload_id", std::string{}).c_str(),
                            item.value("file_name", std::string{}).c_str(),
                            item.value("percent", 0),
                            item.value("state", std::string{}).c_str());
                if (!item.value("final_file_id", std::string{}).empty()) {
                    std::printf("  file %s", item.value("final_file_id", std::string{}).c_str());
                }
                if (item.value("duplicate_count", 1) > 1) {
                    std::printf("  (%llu 条历史)",
                                static_cast<unsigned long long>(
                                    item.value("duplicate_count", 1)));
                }
                std::printf("\n");
            }
            std::printf("共 %zu 个上传文件", uploads->size());
            if (value.data.value("total_sessions", uploads->size()) > uploads->size()) {
                std::printf("（%zu 条历史会话）",
                            static_cast<std::size_t>(
                                value.data.value("total_sessions", uploads->size())));
            }
            std::printf("\n");
        }
        return 0;
    }

    if (verb == "wait") {
        UploadProgressView view;
        while (true) {
            nlohmann::json one;
            one["argv"] = nlohmann::json::array({id});
            Result<ipc::Response> response = upload_call(session, options, "upload.status", one);
            if (!ok(response)) {
                print_failure(*error_of(response));
                return exit_code(error_of(response)->code);
            }
            const ipc::Response& value = std::get<ipc::Response>(response);
            if (!value.ok) {
                print_failure(value.error);
                return exit_code(value.error.code);
            }
            render_upload_progress(value.data, view);
            const std::string state = value.data.value("state", std::string{});
            if (state == "completed") {
                std::printf("\n完成：%s（%s）\n",
                            value.data.value("final_file_id", std::string{}).c_str(),
                            value.data.value("file_name", std::string{}).c_str());
                return 0;
            }
            if (state == "paused") {
                std::printf("\n已暂停；继续：upload resume %s\n", id.c_str());
                return 0;
            }
            if (state == "error" || state == "canceled") {
                std::printf("\n%s\n", value.data.value("error", std::string{}).c_str());
                return exit_code(state == "canceled" ? ErrorCode::UploadCanceled
                                                     : ErrorCode::UploadStateInvalid);
            }
            ::Sleep(view.console ? 250 : 500);
        }
    }

    const std::string operation = "upload." + verb;
    if (verb != "status" && verb != "pause" && verb != "resume" && verb != "cancel") {
        std::fprintf(stderr, "%s\n", usage.c_str());
        return exit_code(ErrorCode::InvalidArgument);
    }
    args["argv"].push_back(id);
    Result<ipc::Response> response = upload_call(session, options, operation, args);
    if (!ok(response)) {
        print_failure(*error_of(response));
        return exit_code(error_of(response)->code);
    }
    const ipc::Response& value = std::get<ipc::Response>(response);
    if (!value.ok) {
        print_failure(value.error);
        return exit_code(value.error.code);
    }
    print_upload_one(value.data);
    return 0;
}

int run_business_command(const std::vector<std::string>& parts, Session& session,
                         const Options& options, bool interactive) {
    if (parts[0] == "upload") {
        return run_upload_command(parts, session, options);
    }
    const std::string operation = parts[0] + "." + parts[1];
    // user 组：口令在 CLI 侧就地哈希 / 走挑战-应答，**明文不进管道**，
    // 与其余业务命令（只发位置参数）的形态不同，所以单独一条路。
    if (parts[0] == "user") {
        return run_user_command(parts, session, options, interactive);
    }

    // 位置参数。--yes / --sort 是本地开关，不发给服务的 argv。
    nlohmann::json arguments = nlohmann::json::array();
    bool confirmed = false;
    std::string sort_key;
    std::string search_key;
    int page = 0;
    int page_size = 0;
    bool show_token = false;  // config list --show-token（本地开关，不发给服务）
    for (std::size_t i = 2; i < parts.size(); ++i) {
        if (parts[i] == "--yes" || parts[i] == "-y") {
            confirmed = true;
            continue;
        }
        if (parts[i] == "--show-token") {
            show_token = true;
            continue;
        }
        if (parts[i] == "--sort" && i + 1 < parts.size()) {
            sort_key = parts[++i];
            continue;
        }
        if ((parts[i] == "--search" || parts[i] == "-s") && i + 1 < parts.size()) {
            search_key = parts[++i];
            continue;
        }
        if (parts[i] == "--page" && i + 1 < parts.size()) {
            page = std::atoi(parts[++i].c_str());
            continue;
        }
        if (parts[i] == "--page-size" && i + 1 < parts.size()) {
            page_size = std::atoi(parts[++i].c_str());
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
    // bucket.delete 也在内——桶里有东西时要提醒「之后只能整体恢复这个桶」；
    // trash.empty 是整站永久删除，同样要确认。
    // trash.restore 也在内：目标 Bucket 已存在时它是**合并**（拿回收站的数据往现有桶里加），
    // 要说清「搬几个文件进去」再让用户确认；没有冲突时预检回 needs_confirm=false，
    // 照旧静默执行——这条门槛因此不妨碍普通回退。
    const bool destructive = (operation == "file.delete" || operation == "trash.delete" ||
                              operation == "bucket.delete" || operation == "trash.empty" ||
                              operation == "trash.restore");
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
    if (!sort_key.empty()) {
        request.args["sort"] = sort_key;  // file list --sort name|size|id
    }
    if (!search_key.empty()) {
        request.args["search"] = search_key;  // file list --search 关键字
    }
    if (page > 0) {
        request.args["page"] = page;  // file list --page 2 --page-size 20
    }
    if (page_size > 0) {
        request.args["page_size"] = page_size;
    }

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
        print_business_data(value.data, show_token);
        // 改了 config/server.json 的操作（本次改动（未提交）：config set server_*）要重启服务
        // 才生效。**由 CLI 来做这件事**：服务进程去提权装自己是在踩自己的脚。
        // 已经提权就当场做（一次 UAC 都不弹）；没提权就弹一次 UAC 拉起提权副本，
        // 用户拒了就如实提示手动执行。
        if (value.data.is_object() && value.data.value("restart_required", false)) {
            restart_service_after_config(options);
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

// ---- 改完 config/server.json 之后把服务重启起来（本次改动（未提交）） ----
//
// 为什么要有它：`server.json` **只在服务启动时读一次**（ServerRuntime::start →
// restart_http），所以 `config set server_enabled / server_host / server_port` 光写文件
// 是不生效的——用户改完发现端口还是没开，就会以为功能坏了（真机上就发生过一次）。
//
// 为什么由 CLI 做、而不是服务自己：让正在跑的服务进程去提权卸载并重装自己，等于在
// 自己脚下抽地板（13.8 节）。CLI 是短命进程，本来就是 service 命令的执行者。
//
// 三条分支：
//   ① 服务没安装 → 不用重启，说明「下次 service install / 双击引导时按新配置起」；
//   ② 已经提权（比如管理员窗口里跑的 CLI）→ 直接 reinstall，一次 UAC 都不弹；
//   ③ 普通权限 → 走 elevate_service_command("reinstall")，弹一次 UAC；
//      用户拒了（FMT-004）就**如实提示手动执行**，绝不假装已经生效。
void restart_service_after_config(const Options& options) {
    const service::State state = service::query_state();
    if (state == service::State::NotInstalled) {
        std::printf("提示：服务尚未安装，本次改动会在下次安装/启动服务时生效\n");
        log_info("Cli", "config 改了 server.json，但服务未安装，无需重启");
        return;
    }

    std::printf("正在重启服务让新配置生效（%s）…\n",
                is_elevated() ? "已提权，直接重装" : "需要一次 UAC 提权");

    if (is_elevated()) {
        // 已经在管理员权限里：不需要再拉起提权副本，直接按当前 exe 重装 + 重启。
        const int code = run_service_command("reinstall", options);
        if (code == 0) {
            std::printf("服务已重启，新配置已生效\n");
            log_info("Cli", "config 改动后已直接 reinstall");
        } else {
            std::fprintf(stderr,
                         "服务重启失败（退出码 %d）：请手动执行 `fmt.exe service reinstall`\n",
                         code);
        }
        return;
    }

    const Result<ElevatedOutcome> elevated =
        elevate_service_command("reinstall", options.self_path);
    if (ok(elevated) && std::get<ElevatedOutcome>(elevated).exit_code == 0) {
        std::printf("服务已重启，新配置已生效\n");
        log_info("Cli", "config 改动后已提权 reinstall");
        return;
    }

    // 提权被拒 / 副本失败：把该敲的命令原样告诉用户，别让他猜。
    std::fprintf(stderr,
                 "未能自动重启服务（提权被取消或失败）：请手动执行 `fmt.exe service reinstall`，\n"
                 "       否则刚才的配置要等服务下次启动才生效\n");
    log_error("Cli", "config 改动后自动 reinstall 未成功");
}

// ---- log clean：日志文件的清理（本地命令，不过服务） ----
//
// 为什么不走服务：日志是**每个数据根各自一份**的本地文件，而且服务可能根本不在跑
// （`--help` / `version` 以外的所有命令都要连服务，日志清理不该有这个前提）。
// 口径与启动时的自动清理完全一致——同一个 cleanup_logs()，所以「手动收一次」与
// 「启动时兜底」不可能给出两套说法。
int run_log_command(const std::vector<std::string>& parts, const Options& options) {
    const std::string usage = "用法：log clean [--keep 8MB] [--dry-run]";
    if (parts.size() < 2) {
        std::fprintf(stderr, "%s\n", usage.c_str());
        return exit_code(ErrorCode::InvalidArgument);
    }
    if (parts[1] != "clean") {
        std::fprintf(stderr, "未知的日志操作：%s\n%s\n", parts[1].c_str(), usage.c_str());
        return exit_code(ErrorCode::InvalidArgument);
    }

    std::uintmax_t budget = kLogBudget;
    bool dry_run = false;
    for (std::size_t i = 2; i < parts.size(); ++i) {
        if (parts[i] == "--dry-run") {
            dry_run = true;
            continue;
        }
        if (parts[i] == "--keep") {
            if (i + 1 >= parts.size()) {
                std::fprintf(stderr, "--keep 后面要跟大小，例如 --keep 8MB\n");
                return exit_code(ErrorCode::InvalidArgument);
            }
            if (!parse_log_budget(parts[++i], &budget)) {
                std::fprintf(stderr, "无法识别的大小：%s（可写 8388608 / 8MB / 512KB / 1GB）\n",
                             parts[i].c_str());
                return exit_code(ErrorCode::InvalidArgument);
            }
            continue;
        }
        std::fprintf(stderr, "未知参数：%s\n%s\n", parts[i].c_str(), usage.c_str());
        return exit_code(ErrorCode::InvalidArgument);
    }

    const PathManager paths{path_from_utf8(options.data_root)};
    const LogDirectoryInfo before = inspect_log_directory(paths.log());
    if (!before.exists) {
        std::printf("日志目录还不存在，无需清理：%s\n", path_to_utf8(paths.log()).c_str());
        std::printf("执行成功...\n错误码：0\n");
        return 0;
    }

    const Result<LogCleanup> cleaned = cleanup_logs(paths.log(), budget, dry_run);
    if (!ok(cleaned)) {
        const Error& error = *error_of(cleaned);
        log_error("Log", "日志清理失败：" + error.message);
        print_failure(error);
        return exit_code(error.code);
    }
    const LogCleanup& report = std::get<LogCleanup>(cleaned);

    std::printf("日志目录：%s\n", path_to_utf8(paths.log()).c_str());
    std::printf("当前占用：%s（%zu 个文件）\n", format_size(before.bytes).c_str(), before.files);
    std::printf("保留上限：%s\n", format_size(budget).c_str());
    if (!report.over_budget) {
        std::printf("未超过上限，未做任何清理。\n");
    } else {
        std::printf("%s\n", report.message.c_str());
        for (const std::string& note : report.notes) {
            std::printf("  %s\n", note.c_str());
        }
        if (dry_run) {
            std::printf("（这是预演：磁盘一个字节都没动；去掉 --dry-run 才会真的清理）\n");
        }
    }
    std::printf("执行成功...\n错误码：0\n");

    // 走到这里才写日志：清理本身的结果进**清理之后**的文件，用户翻日志看得到。
    // （--dry-run 与「本来就没事」都不记：那种行只会把日志越写越大。）
    if (report.over_budget) {
        log_info("Log", std::string(dry_run ? "log clean --dry-run：" : "log clean：") +
                            report.message);
    }
    return 0;
}

// ---- 交互循环 ----
int run_interactive(const Options& options, service::State state, Session& session) {
    std::printf("%s\n", banner_text().c_str());
    std::printf("%s\n", service_state_line(state).c_str());

    // 数据根就是「服务在看哪个目录」。**永远显示一行**：这是排查问题时最要紧的信息，
    // 以前只写在日志里，用户得去翻文件。与本程序所在目录不一致时再补一句说明
    //（连上之后服务会切到本目录——数据根由 CLI 声明）。
    if (Result<service::ServiceState> loaded = service::load_state(); ok(loaded)) {
        const std::string service_root = std::get<service::ServiceState>(loaded).current_root;
        if (!service_root.empty()) {
            std::printf("数据根：%s\n", service_root.c_str());
            if (!iequals(service_root, to_forward_slashes(options.data_root))) {
                std::printf(
                    "注意：本程序所在目录是 %s，连上之后服务会切到本目录"
                    "（数据根由 CLI 声明）。\n",
                    to_forward_slashes(options.data_root).c_str());
            }
        }
    }
    log_info("Cli", "进入交互循环，数据根：" + to_forward_slashes(options.data_root));

    // 首次启动引导（只在这里、只有交互模式做）：还没有账号时先要求建账号。
    //
    // 为什么只拦交互模式：一次性命令是脚本用的，卡在提示上会让脚本挂住；
    // 它们本来就在下一层拿到「还没有账号」的明确错误（FMT-002 + 提示）。
    bool need_account = false;
    {
        const PathManager paths(std::filesystem::path(fmt::path_from_utf8(options.data_root)));
        Config probe_config;
        UserStore store(paths, probe_config);
        if (Result<UserRecord> loaded = store.load(); ok(loaded)) {
            need_account = std::get<UserRecord>(loaded).empty();
        }
    }
    if (need_account) {
        std::printf("\n%s\n", fmt::user::UserService::first_run_hint().c_str());
    }

    // `fmt -u <用户名> -p` 进交互模式：先当场登录，再进循环。
    // `fmt -u <用户名> -p` 进交互模式：先当场登录，再进循环。
    //
    // **已经登录过就不再问一遍**：`-u/-p` 不带命令时，dispatch_command 已经把口令
    // 问过一次并验过了（那时才走到 bootstrap_and_run），这里再问就是重复输入。
    // 真踩过：这条判断缺失导致"登录完窗口里什么都没发生"。
    if (options.login_requested && !session.logged_in) {
        std::string password;
        if (read_password_masked("请输入密码：", &password)) {
            if (const Status status = ensure_connected(session, options); !ok(status)) {
                print_failure(*error_of(status));
            } else {
                const Result<std::string> who =
                    verify_login(session, options, options.login_user, password,
                                 &session.password_digest);
                if (ok(who)) {
                    session.logged_in = true;
                    session.username = std::get<std::string>(who);
                    std::printf("登录成功：%s\n", session.username.c_str());
                } else {
                    print_failure(*error_of(who));
                    if (error_of(who)->code == ErrorCode::FileNotFound) {
                        std::fprintf(stderr, "这个数据根还没有账号，请先创建：user create <用户名>\n");
                    }
                }
            }
            password.assign(password.size(), '\0');
        }
    }

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
        // 窗口内登录/登出（真机反馈：`-u/-p` 只在启动参数里有效，进了提示符就不认了，
        // 于是用户会去敲 `fmt -u lazy -p`，然后被门禁当成"未知命令组"。）
        if (head == "login") {
            std::string who = parts.size() >= 2 ? parts[1] : std::string{};
            if (const Status status = ensure_connected(session, options); !ok(status)) {
                print_failure(*error_of(status));
                continue;
            }
            if (who.empty()) {
                // 不带用户名：用账号里那个（还没账号就明确提示）。
                ipc::Request request;
                request.root = options.data_root;
                request.pid = GetCurrentProcessId();
                request.op = "user.get";
                request.id = session.next_id++;
                const Result<ipc::Response> existing =
                    session.client.call(request, ipc::kCommandTimeoutMs);
                if (!ok(existing) || !std::get<ipc::Response>(existing).ok) {
                    std::printf("还没有账号，请先创建：user create <用户名>\n");
                    continue;
                }
                who = std::get<ipc::Response>(existing).data.value("username", std::string{});
            }
            std::string password;
            if (!read_password_masked("请输入密码：", &password)) {
                continue;
            }
            const Result<std::string> verified =
                verify_login(session, options, who, password, &session.password_digest);
            password.assign(password.size(), '\0');
            if (ok(verified)) {
                session.logged_in = true;
                session.username = std::get<std::string>(verified);
                std::printf("登录成功：%s\n", session.username.c_str());
            } else {
                print_failure(*error_of(verified));
                if (error_of(verified)->code == ErrorCode::FileNotFound) {
                    std::printf("还没有账号，请先创建：user create <用户名>\n");
                }
            }
            continue;
        }
        if (head == "logout") {
            if (!session.logged_in) {
                std::printf("当前没有登录。\n");
                continue;
            }
            std::printf("已退出登录：%s（业务命令现在需要重新登录）\n", session.username.c_str());
            session.logged_in = false;
            session.username.clear();
            continue;
        }
        if (head == "log") {
            run_log_command(parts, options);  // 本地清理：不需要服务
            continue;
        }
        if (head == "service") {
            if (parts.size() >= 2 && parts[1] == "status") {
                show_service_status();  // 查询不需要提权
                continue;
            }
            if (parts.size() < 2 || !is_user_service_command(parts[1])) {
                std::fprintf(stderr, "用法：service install | uninstall | start | stop | reinstall | status\n");
                continue;
            }
            run_service_command(parts[1], options);
            continue;
        }
        if (parts.size() < 2) {
            std::fprintf(stderr, "未知命令：%s（输入 help 查看用法）\n", head.c_str());
            continue;
        }

        // 首次启动：还没有账号时先把用户请到 `user create`，其余命令一律不执行。
        // 只有 `user`、`help`、`service`、`log` 这几个「与账号数据无关」的放行。
        if (need_account && head != "user") {
            std::printf("这个数据根还没有账号，请先执行：user create <用户名>\n");
            continue;
        }

        // 登录门禁（用户要求）：业务命令必须先 `fmt -u <用户名> -p` 登录。
        // **service / log / help / version / exit 不受限**——服务坏了、账号进不去的时候，
        // 还得靠它们把机器救回来。
        //
        // 三条例外，都是「不能逼人先登录」的：
        //   * `user create`：还没有账号时得先能建出来；
        //   * `user update password`：本身凭密码自证（会问当前密码），
        //     逼用户先 login 等于把同一个密码输两遍；
        //   * `user delete`：**忘记密码时唯一的逃生门**（用户明确要求）。
        // 其余 user 子命令（get / token renew）都要登录。
        const bool user_readonly_ok =
            (head == "user" && parts.size() >= 2 &&
             (parts[1] == "create" || parts[1] == "update" || parts[1] == "delete"));
        if (!session.logged_in && !user_readonly_ok && !business_allowed(head, !need_account)) {
            // 提示要**能照着做**：窗口里直接 `login` 就行。
            // 曾经写的是「请重新打开窗口并登录：fmt -u <用户名> -p」——那时候窗口内
            // 还不能登录；现在能了，再说"重新打开窗口"就是把用户往门外推。
            std::printf(
                "需要先登录才能执行 %s。\n"
                "本窗口内登录：login <用户名>      （回车后隐藏输入密码）\n"
                "不需要登录：service / log / help / version / user create / user update password / user delete\n",
                command_label(parts).c_str());
            continue;
        }

        run_business_command(parts, session, options, true);

        // user create 成功之后要放行其余命令：重新看一眼账号在不在。
        if (need_account && head == "user") {
            const PathManager paths(std::filesystem::path(fmt::path_from_utf8(options.data_root)));
            Config probe_config;
            UserStore store(paths, probe_config);
            if (Result<UserRecord> loaded = store.load();
                ok(loaded) && !std::get<UserRecord>(loaded).empty()) {
                need_account = false;
                std::printf("账号已就绪，可以开始使用了（输入 help 看命令）。\n");
            }
        }
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
    // **测试闸门**：FMT_NO_SERVICE=1 时完全不碰服务管理（不装、不启、不重装），
    // 只当服务已经在跑。为什么必须有它：端到端测试要用无参数方式跑真的 fmt.exe，
    // 那就走进双击引导，而引导会去操作**真实的 Windows 服务**——FMT_PIPE 只隔离
    // 管道，管不到 SCM。曾经因此把服务注册指向测试临时目录，宿主 exe 随后被清理，
    // 线上服务指向不存在的文件（真事故，见提交信息）。
    if (env_flag("FMT_NO_SERVICE")) {
        log_info("Service", "FMT_NO_SERVICE=1：跳过全部服务管理（测试模式）");
        Session session;
        const service::State state = service::query_state();
        if (state == service::State::Running) {
            if (const Status status = ensure_connected(session, options); !ok(status)) {
                log_warn("Cli", "暂时无法把数据根声明给服务：" + error_of(status)->message);
            }
        }
        return run_interactive(options, state, session);
    }

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

int dispatch_command(const std::vector<std::string>& args, const Options& options) {
    // 一次性命令不参与单实例：已经开着一个窗口时，别的脚本仍然要能停服务。
    if (!args.empty()) {
        // ---- 先摘掉 `-u <用户名> -p`：登录开关，不是命令 ----
        //
        // `-p` 后面**不接值**：回车后隐藏输入（用户要求「-p 回车输入密码」）。
        // 剩下的才是真正的命令，例如 `fmt -u lazy -p file list`。
        std::vector<std::string> rest;
        std::string login_user;
        bool want_login = false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "-u" || args[i] == "--user") {
                if (i + 1 >= args.size()) {
                    std::fprintf(stderr, "用法：fmt -u <用户名> -p\n");
                    return exit_code(ErrorCode::InvalidArgument);
                }
                login_user = args[++i];
                continue;
            }
            if (args[i] == "-p" || args[i] == "--password") {
                want_login = true;
                continue;
            }
            rest.push_back(args[i]);
        }
        const bool has_command = !rest.empty();

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
                std::fprintf(stderr, "用法：service install | uninstall | start | stop | reinstall | status\n");
                return exit_code(ErrorCode::InvalidArgument);
            }
            return run_service_command(args[1], options);
        }
        if (args[0] == "log") {
            return run_log_command(args, options);  // 本地清理：不需要服务
        }

        // 从这里开始是业务命令：**必须先登录**（用户要求）。
        //
        // 登录态只在本次进程的内存里；一次性命令用完就退出，所以要登录就得在同一次
        // 调用里带上 `-u/-p`。
        Session session;
        bool logged_in = false;

        // 只给了登录开关、没有命令 → **进交互窗口**（用户要的正是这个用法：
        // `fmt -u lazy -p` 回车输入密码后就待在窗口里干活）。
        // 这里不能 return：那会让"登录完就退出"，窗口里什么都没发生。
        if (!has_command && want_login) {
            return bootstrap_and_run(options);
        }

        if (!has_command) {
            std::fprintf(stderr, "未知命令：%s\n\n", args[0].c_str());
            print_usage();
            return exit_code(ErrorCode::InvalidArgument);
        }

        if (want_login) {
            std::string password;
            if (!read_password_masked("请输入密码：", &password)) {
                return exit_code(ErrorCode::InvalidArgument);
            }
            if (const Status status = ensure_connected(session, options); !ok(status)) {
                print_failure(*error_of(status));
                return exit_code(error_of(status)->code);
            }
            const Result<std::string> who =
                verify_login(session, options, login_user, password, &session.password_digest);
            password.assign(password.size(), '\0');
            if (!ok(who)) {
                print_failure(*error_of(who));
                return exit_code(error_of(who)->code);
            }
            logged_in = true;
            session.logged_in = true;
            session.username = std::get<std::string>(who);
            std::printf("登录成功：%s\n", session.username.c_str());
        }

        if (!logged_in) {
            // 例外与交互模式一致：`user create`（还没有账号时得先能建）与
            // `user get`（看账号状态）不需要登录。
            const bool user_readonly_ok =
                (rest[0] == "user" && rest.size() >= 2 &&
                 (rest[1] == "create" || rest[1] == "update" || rest[1] == "delete"));
            if (!user_readonly_ok) {
                const std::string label = command_label(rest);
                std::fprintf(stderr,
                             "需要先登录才能执行 %s。\n"
                             "用法：fmt -u <用户名> -p %s\n"
                             "（口令在回车后隐藏输入；登录态只活在本次命令/窗口里）\n",
                             label.c_str(), label.c_str());
                return exit_code(ErrorCode::Unauthorized);
            }
        }

        if (rest.size() >= 2) {
            return run_business_command(rest, session, options, false);
        }
        std::fprintf(stderr, "未知命令：%s\n\n", rest[0].c_str());
        print_usage();
        return exit_code(ErrorCode::InvalidArgument);
    }

    // 双击：只留一个 CLI 窗口。
    //
    // 互斥体名可以用 FMT_MUTEX 覆盖：端到端测试要能**同时**开着用户的 FMT 窗口
    // 跑无参数交互式子进程，否则会撞上这个单实例、子进程静默退出（真踩过：
    // 一条交互式用例因此长时间"输出里只有登录成功"）。与 FMT_PIPE / FMT_NO_SERVICE
    // 同一个套路——只为测试，正常使用不设这个变量。
    SetConsoleTitleW(kConsoleTitle);
    std::wstring mutex_name = kSingletonMutex;
    if (const char* override_name = std::getenv("FMT_MUTEX");
        override_name != nullptr && *override_name != '\0') {
        mutex_name = L"Local\\" + to_wide(override_name);
    }
    HANDLE singleton = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (singleton != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已经有一个 FMT 窗口：把它拉到前面，本次静默退出（有真控制台时才加一句说明）。
        //
        // 为什么在**有控制台窗口**时说一句：以前这里完全不打印，于是「双击后窗口一闪就没了」
        // 只能靠翻日志才能解释（而日志本身又可能因为数据根不可写而不存在）。
        // 只在 GetConsoleWindow() 非空时打印——脚本/自动化（管道 stdin，没有控制台）不受影响。
        const bool activated = activate_existing_window();
        if (GetConsoleWindow() != nullptr) {
            std::fprintf(stderr, activated
                                     ? "FMT 已经开着一个窗口，已把它切到前面（本次不再新开窗口）\n"
                                     : "FMT 已经开着一个窗口，本次不再新开窗口（未能把它切到前面）\n");
        }
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

    // `-u <用户名> -p`：**双击/无参数**时的登录开关。带命令的形式在
    // dispatch_command 里自己解析（那份还要拆出真正的命令来）。
    //
    // 只在这里改 options 的副本：调用方（run）按值收，改完继续往下走。
    Options effective = options;
    std::vector<std::string> remaining;
    {
        bool saw_user = false;
        bool saw_password = false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if ((args[i] == "-u" || args[i] == "--user") && i + 1 < args.size()) {
                effective.login_user = args[i + 1];
                saw_user = true;
                ++i;
                continue;
            }
            if (args[i] == "-p" || args[i] == "--password") {
                saw_password = true;
                continue;
            }
            remaining.push_back(args[i]);
        }
        effective.login_requested = saw_user && saw_password;
        // 带命令时把开关留给 dispatch_command（它要在同一个 Session 里登录+执行）。
        if (!remaining.empty()) {
            effective.login_requested = false;
        }
    }

    // 双击：先把数据根补齐（含 log/），再开日志器——这样「新建目录」的清单是
    // 完整的六个，这些行也不会因为日志器还没开而丢掉。
    std::vector<std::string> root_notes;
    if (args.empty()) {
        prepare_data_root(effective, &root_notes);
    }

    // 日志清理：**在开日志器之前**（日志器一开就握着文件句柄，之后再删备份/替换
    // 活动文件会被共享冲突挡住）。一条命令的执行也先兜底清一次，代价是几次
    // file_size——不超预算时一个字节都不动。见 §65 与 help log。
    //
    // **显式 `log clean` 时跳过这一步**：否则自动清理会把目录先收干净，紧接着
    // 用户那条命令（尤其是 `--dry-run`）就只剩「未超过上限」可报——预演看不到
    // 真相。那一次由命令自己按用户给的预算清理，口径只有一份。
    const bool explicit_log_command = !args.empty() && args[0] == "log";
    const PathManager log_paths{path_from_utf8(effective.data_root)};
    Result<LogCleanup> cleaned = make_error(ErrorCode::InvalidArgument, "未清理");
    bool log_cleaned = false;
    if (!explicit_log_command) {
        cleaned = cleanup_logs(log_paths.log(), kLogBudget);
        log_cleaned = ok(cleaned) && std::get<LogCleanup>(cleaned).over_budget;
    }

    // 从这里开始都是真的干活，才值得写日志：--help / --version 不该在磁盘上
    // 留下任何东西。日志与 Service 共用同一个 <数据根>/log/fmt.log。
    std::unique_ptr<Logger> file_logger;
    if (Result<std::unique_ptr<Logger>> opened = open_cli_logger(effective.data_root); ok(opened)) {
        file_logger = std::move(std::get<std::unique_ptr<Logger>>(opened));
        set_logger(file_logger.get());
    }

    if (log_cleaned) {
        // 清理的结果写进**清理之后**的文件（顺序：先清、再开日志器、再报告）。
        const LogCleanup& report = std::get<LogCleanup>(cleaned);
        log_info("Log", report.message);
        for (const std::string& note : report.notes) {
            log_info("Log", note);
        }
    }

    for (const std::string& note : root_notes) {
        log_info("Cli", note);
    }

    std::string summary = "CLI 启动 v" + std::string(version::STRING) +
                          "，数据根：" + to_forward_slashes(effective.data_root);
    if (args.empty()) {
        summary += "，交互模式";
    } else {
        summary += "，命令：" + args[0];
        if (args.size() > 1) {
            summary += " " + args[1];
        }
    }
    log_info("Cli", summary);
    note_service_root(effective);

    const int code = dispatch_command(args, effective);

    log_info("Cli", "CLI 退出，错误码 " + std::to_string(code));
    set_logger(nullptr);  // 先摘掉指针，再让 file_logger 析构
    return code;
}

// 版本文本的**唯一来源**：横幅、--version、version 命令都走它。
// 必须定义在匿名命名空间**外面**（banner_text 在里面，但那里面的名字对外不可见，
// 声明在 cli.hpp 里的函数不能跟着进去——否则链接期找不到符号）。
std::string version_text() { return banner_text(); }

std::string root_switch_notice(const std::string& previous, const std::string& current) {
    std::string text = "\n注意：服务的数据根已切换\n";
    text += "  原来：" + previous + "\n";
    text += "  现在：" + current + "\n";
    text += "原因是「数据根由 CLI 声明」：谁连上服务，服务就用谁的目录。\n";
    text += "如果这不是你想要的，请用数据根正确的那个 fmt.exe 再执行一条命令切回去。\n\n";
    return text;
}

// 用户可以直接键入的 service 子命令 —— **每条都要提权**（`status` 不在其中）。
// reinstall 原来只是双击引导的内部用法，现在也是正式命令：它是「换服务宿主 exe」
// 唯一一条**一次 UAC** 的路径（卸载 + 按当前这个 exe 重装并启动）。
bool is_user_service_command(const std::string& operation) {
    return operation == "install" || operation == "uninstall" || operation == "start" ||
           operation == "stop" || operation == "reinstall";
}

}  // namespace fmt::cli