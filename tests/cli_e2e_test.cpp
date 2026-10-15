// CLI 端到端冒烟测试：用**真实的 fmt.exe**、走**真实命名管道**，打到一个进程内服务上。
//
// 为什么要有这一层：`file delete` 弹出 abort 的那次崩溃，单元测试**全绿**却没挡住——
// 因为测试直接调业务层、自己按服务端期望的形状拼请求，而 CLI 拼的是另一种形状。
// 只有让真实的 exe 走一遍用户走的路（含交互式确认），这类回归才会当场露出来。
//
// 与真服务隔离：测试进程把管道名换成 `FMT_PIPE` 指定的唯一名字，它自己起的服务和
// 它拉起的 fmt.exe 子进程都用这个名字。所以**用户机器上真服务正在跑也不受影响**，
// 更不会把命令打到真数据上。
#include "fmt/cli/cli.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/ipc/pipe.hpp"
#include "fmt/service/runtime.hpp"
#include "fmt/share/share.hpp"  // kDefaultShareDownloads（把次数下满那一段用）
#include "fmt/storage/storage.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

namespace {

struct CliRun {
    int exit_code = -1;
    std::string output;  // stdout 与 stderr 合并（按用户看到的样子判断）
};

std::string to_utf8_bytes(const std::string& text) { return text; }

// 把命令行按 CreateProcessW 的规矩拼：每个参数加引号，内部引号转义。
std::wstring build_command_line(const std::filesystem::path& exe,
                                const std::vector<std::string>& args) {
    std::wstring line = L"\"" + exe.wstring() + L"\"";
    for (const std::string& arg : args) {
        line += L" \"";
        for (const wchar_t ch : fmt::to_wide(arg)) {
            if (ch == L'"') {
                line += L"\\\"";  // 简单转义：我们的参数里不含反斜杠结尾
            } else {
                line += ch;
            }
        }
        line += L"\"";
    }
    return line;
}

// 起一个 fmt.exe，喂 stdin，收 stdout+stderr，拿退出码。
CliRun run_cli(const std::filesystem::path& exe, const std::vector<std::string>& args,
               const std::string& stdin_text) {
    CliRun result;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    HANDLE child_output_read = nullptr;
    HANDLE child_output_write = nullptr;
    if (!CreatePipe(&child_stdin_read, &child_stdin_write, &attributes, 0) ||
        !CreatePipe(&child_output_read, &child_output_write, &attributes, 0)) {
        return result;
    }
    // 父进程这一侧不要被子进程继承
    SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_output_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin_read;
    startup.hStdOutput = child_output_write;
    startup.hStdError = child_output_write;

    PROCESS_INFORMATION process{};
    std::wstring command = build_command_line(exe, args);
    const BOOL started =
        CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       exe.parent_path().wstring().c_str(), &startup, &process);

    // 父进程不再需要子进程那两端的句柄，否则读管道不会因为子进程退出而结束
    CloseHandle(child_stdin_read);
    CloseHandle(child_output_write);
    if (!started) {
        CloseHandle(child_stdin_write);
        CloseHandle(child_output_read);
        return result;
    }

    // 诊断：单实例互斥体是不是被别的进程占着。CLI 在没有命令行参数时走「双击」分支，
    // 发现已有窗口就**静默退出（退出码 0、零输出）**——测试里遇到这种就会表现成
    // 「明明该打印却什么都没打印」。把事实测出来，别去猜。
    {
        SetLastError(ERROR_SUCCESS);  // 不平：CreateMutexW 只在新建时报 ERROR_ALREADY_EXISTS
        HANDLE probe = CreateMutexW(nullptr, FALSE, L"Local\\FMT.CLI.v1");
        const bool busy = (probe != nullptr && GetLastError() == ERROR_ALREADY_EXISTS);
        if (probe != nullptr) {
            CloseHandle(probe);
        }
        if (busy) {
            std::fprintf(stderr,
                         "[e2e] 警告：FMT.CLI.v1 单实例互斥体已被占用——子进程会静默退出，"
                         "本次断言多半会失败（先确认没有别处开着的 FMT 窗口）\n");
        }
    }

    if (!stdin_text.empty()) {
        DWORD written = 0;
        WriteFile(child_stdin_write, stdin_text.data(), static_cast<DWORD>(stdin_text.size()),
                  &written, nullptr);
    }
    CloseHandle(child_stdin_write);  // 关掉 stdin：交互循环读到 EOF 会自己退出

    const DWORD wait = WaitForSingleObject(process.hProcess, 120000);
    // 把管道里剩下的读完（进程已退出，读不会阻塞）
    std::string output;
    char buffer[4096];
    DWORD available = 0;
    while (PeekNamedPipe(child_output_read, nullptr, 0, nullptr, &available, nullptr) &&
           available > 0) {
        DWORD read = 0;
        if (!ReadFile(child_output_read, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            break;
        }
        output.append(buffer, read);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = (wait == WAIT_OBJECT_0) ? static_cast<int>(exit_code) : -1;
    result.output = to_utf8_bytes(output);

    CloseHandle(child_output_read);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

bool has(const CliRun& run, const std::string& needle) {
    return run.output.find(needle) != std::string::npos;
}

// 从输出里抓第一个 file_id（fmt-YYYYMMDD-N）。
std::string grab_file_id(const CliRun& run) {
    const std::size_t at = run.output.find("fmt-");
    if (at == std::string::npos) {
        return {};
    }
    std::size_t end = at;
    while (end < run.output.size()) {
        const char ch = run.output[end];
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || ch == '-') {
            ++end;
        } else {
            break;
        }
    }
    return run.output.substr(at, end - at);
}

// 从输出里抓第一个 share_id（12 位小写十六进制，跟着标号「share_id：」）。
std::string grab_share_id(const CliRun& run) {
    const std::size_t at = run.output.find("share_id：");
    if (at == std::string::npos) {
        return {};
    }
    const std::size_t begin = at + std::string("share_id：").size();
    std::size_t end = begin;
    while (end < run.output.size()) {
        const char ch = run.output[end];
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!hex) {
            break;
        }
        ++end;
    }
    return run.output.substr(begin, end - begin);
}

// 一个隔离的端到端环境：唯一管道名 + 独立数据根（fmt.exe 就放在里面，因为
// 「数据根 = CLI 所在目录」）+ 进程内服务。
class E2eFixture {
public:
    E2eFixture() {
        static std::atomic<unsigned> counter{0};
        pipe_ = L"\\\\.\\pipe\\fmt-e2e-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(counter.fetch_add(1));
        SetEnvironmentVariableW(L"FMT_PIPE", pipe_.c_str());
        // 必须同时告诉子进程「别碰服务管理」。测试用**无参数**跑真的 fmt.exe 会走进
        // 双击引导，而引导会装/启动/重装**真实的 Windows 服务**——FMT_PIPE 只隔离
        // 管道，管不到 SCM。我们就这样把服务注册指向过测试的临时目录，第二天宿主
        // exe 被清理，线上服务指向不存在的文件。设了这个开关，引导一步都不碰 SCM。
        SetEnvironmentVariableW(L"FMT_NO_SERVICE", L"1");

        FMT_CHECK(fmt::ok(fmt::ensure_directory(deploy_)));
        const std::filesystem::path source = fmt::executable_path().parent_path() / L"fmt.exe";
        FMT_CHECK(fmt::file_exists(source));
        std::error_code code;
        std::filesystem::copy_file(source, deploy_ / L"fmt.exe",
                                   std::filesystem::copy_options::overwrite_existing, code);
        FMT_CHECK(!code);

        // 素材
        FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(temp_.path() / "first.txt", "first")));
        FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(temp_.path() / "second.txt", "second")));

        runtime_ = std::make_unique<fmt::service::ServerRuntime>(deploy_, temp_.path() / "state");
        FMT_CHECK(fmt::ok(runtime_->start()));
        runner_ = std::thread([this] { runtime_->run(); });

        // 等 accept 循环**真的**开始监听再交出去。原来只 sleep 300ms：机器一忙，
        // 第一条命令就撞上「还没起来」，表现成「没出现确认提示」这种看起来像功能
        // 坏了的抖动（本机实测偶发过一次），很难查。
        for (int attempt = 0; attempt < 100; ++attempt) {
            fmt::Result<fmt::ipc::PipeClient> probe =
                fmt::ipc::PipeClient::connect(100, pipe_.c_str());
            if (fmt::ok(probe)) {
                return;  // 连上就说明在监听了（这次连接不带请求，服务端会当作断开继续）
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        FMT_CHECK(false);  // 5 秒还起不来：就地失败，别让它变成后面的诡异现象
    }

    ~E2eFixture() {
        runtime_->request_stop();
        if (runner_.joinable()) {
            runner_.join();
        }
        SetEnvironmentVariableW(L"FMT_PIPE", nullptr);
        SetEnvironmentVariableW(L"FMT_NO_SERVICE", nullptr);
    }

    E2eFixture(const E2eFixture&) = delete;
    E2eFixture& operator=(const E2eFixture&) = delete;

    CliRun cli(const std::vector<std::string>& args, const std::string& stdin_text = {}) const {
        return run_cli(deploy_ / L"fmt.exe", args, stdin_text);
    }

    std::filesystem::path source(const std::string& name) const { return temp_.path() / name; }
    const std::filesystem::path& root() const { return deploy_; }

private:
    fmt_test::TempDir temp_{"cli-e2e"};
    std::filesystem::path deploy_ = temp_.path() / "deploy";
    std::wstring pipe_;
    std::unique_ptr<fmt::service::ServerRuntime> runtime_;
    std::thread runner_;
};

// 交互式跑一次，**输出为空就重跑**（最多三次）。
//
// 为什么要有这个：本机实测大约几十次里有一次，交互式子进程会「退出码 0、一个字节都没输出」
// ——不是产品行为，也不是断言写错（CLI 一旦真的进交互循环，横幅与每一条命令的输出都是
// 确定性的）。已排除的原因：单实例互斥体没被占用（run_cli 里有探针会喊）、子进程创建失败
// （那样退出码是 -1）、临时数据根没建出来（每次都会新建）。所以这更像本机偶发的子进程
// 启动竞态。处置：**重跑一次**，而不是把断言放宽——放宽会把真正的「什么都不打印」放过去。
CliRun run_interactive_until_output(const E2eFixture& fixture, const std::string& stdin_text) {
    CliRun run = fixture.cli({}, stdin_text);
    for (int attempt = 0; attempt < 2 && run.output.empty(); ++attempt) {
        std::fprintf(stderr, "[e2e] 交互式子进程这次没有任何输出（退出码 %d），重跑一次\n",
                     run.exit_code);
        run = fixture.cli({}, stdin_text);
    }
    return run;
}

}  // namespace

FMT_TEST(CliE2e, 核心链路走真实exe与真实管道) {
    E2eFixture fixture;

    // ---- 不需要服务的本地命令 ----
    const CliRun version = fixture.cli({"version"});
    FMT_CHECK_EQ(version.exit_code, 0);
    FMT_CHECK(has(version, "File Manager Tool"));

    // ---- Bucket ----
    const CliRun create = fixture.cli({"bucket", "create", "WORK"});
    FMT_CHECK_EQ(create.exit_code, 0);
    FMT_CHECK(has(create, "work"));      // 统一小写
    FMT_CHECK(has(create, "小写"));       // 并且提示了用户

    const CliRun buckets = fixture.cli({"bucket", "list"});
    FMT_CHECK_EQ(buckets.exit_code, 0);
    FMT_CHECK(has(buckets, "work"));

    // ---- 上传（本地路径）----
    const CliRun upload = fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt"))});
    FMT_CHECK_EQ(upload.exit_code, 0);
    FMT_CHECK(has(upload, "first.txt"));
    const std::string first_id = grab_file_id(upload);
    FMT_CHECK(!first_id.empty());

    // 同内容 + **不同文件名**：同一个桶里同一份内容只留一份 → FMT-304「文件已存在」，
    // **不提示改文件名**（用户的四条口径之一）
    const CliRun same_content =
        fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt")), "first-copy.txt"});
    FMT_CHECK_EQ(same_content.exit_code, 4);
    FMT_CHECK(has(same_content, "FMT-304"));
    FMT_CHECK(has(same_content, "文件已存在"));
    FMT_CHECK(has(same_content, "first.txt"));        // 说清是哪一份
    FMT_CHECK(!has(same_content, "修改文件名"));       // 但不要求改名

    // 同内容 + 同文件名：同样 FMT-304「文件已存在」，同样不提示改文件名
    const CliRun duplicate =
        fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt")), "first.txt"});
    FMT_CHECK_EQ(duplicate.exit_code, 4);
    FMT_CHECK(has(duplicate, "FMT-304"));
    FMT_CHECK(has(duplicate, "文件已存在"));
    FMT_CHECK(!has(duplicate, "修改文件名"));

    // 不同内容 + 同文件名：FMT-105「文件名重复」，这次**要提示改文件名**
    const CliRun conflict =
        fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("second.txt")), "first.txt"});
    FMT_CHECK_EQ(conflict.exit_code, 4);
    FMT_CHECK(has(conflict, "FMT-105"));
    FMT_CHECK(has(conflict, "文件名重复"));
    FMT_CHECK(has(conflict, "修改文件名"));

    // ---- 查询 ----
    const CliRun list = fixture.cli({"file", "list"});
    FMT_CHECK_EQ(list.exit_code, 0);
    FMT_CHECK(has(list, "first.txt"));

    const CliRun by_name = fixture.cli({"file", "get", "first.txt"});
    FMT_CHECK_EQ(by_name.exit_code, 0);
    FMT_CHECK(has(by_name, first_id));

    const CliRun by_id = fixture.cli({"file", "get", first_id});
    FMT_CHECK_EQ(by_id.exit_code, 0);
    FMT_CHECK(has(by_id, "first.txt"));
    // 预览 / 下载链接（本次改动（未提交））：完整 http:// 地址，HTTP 服务开着也照样给
    FMT_CHECK(has(by_id, "预览链接：http://localhost:4122/api/file/" + first_id + "/preview"));
    FMT_CHECK(has(by_id, "下载链接：http://localhost:4122/api/file/" + first_id + "/download"));

    const CliRun missing = fixture.cli({"file", "get", "nope.txt"});
    FMT_CHECK_EQ(missing.exit_code, 3);
    FMT_CHECK(has(missing, "FMT-002"));

    // ---- config list：config.json 的字段 + 网络服务三项（本次改动（未提交））----
    // 只读！`config set server_*` 会触发「改完自动重启服务」（CLI 侧提权重装），
    // 在端到端用例里那会去动**真实的 Windows 服务**（提权弹窗也挡不住断言），
    // 所以这里只断言 `config list` 的输出格式；写入路径由 Service.* 单测覆盖。
    const CliRun config = fixture.cli({"config", "list"});
    FMT_CHECK_EQ(config.exit_code, 0);
    // 两个文件按顺序分开：config.json 在前、server.json 在后
    const std::size_t config_at = config.output.find("配置文件：config/config.json");
    const std::size_t server_at = config.output.find("网络服务配置：config/server.json");
    FMT_CHECK(config_at != std::string::npos);
    FMT_CHECK(server_at != std::string::npos);
    FMT_CHECK(config_at < server_at);
    // 同一个文件里的字段同一套后缀风格，且一行一个
    FMT_CHECK(has(config, "当前用户：user"));
    FMT_CHECK(has(config, "当前 Bucket："));
    FMT_CHECK(has(config, "大小单位：MB"));
    FMT_CHECK(has(config, "语言：zh-CN"));
    // 一行只放一个：这两个字段必须在不同的行上
    {
        const std::size_t unit_at = config.output.find("大小单位：");
        const std::size_t lang_at = config.output.find("语言：");
        const std::size_t unit_end = config.output.find('\n', unit_at);
        FMT_CHECK(unit_at != std::string::npos);
        FMT_CHECK(lang_at != std::string::npos);
        FMT_CHECK(unit_end != std::string::npos);
        FMT_CHECK(lang_at > unit_end);
    }
    FMT_CHECK(has(config, "网络服务 enabled："));
    FMT_CHECK(has(config, "服务地址 host：localhost"));
    FMT_CHECK(has(config, "服务端口 port：4122"));
    // token 默认不打印（只在 --show-token 时才出现）
    FMT_CHECK(has(config, "访问 token：（已隐藏"));
    FMT_CHECK(!has(config, "HTTP 请求头："));
    const CliRun with_token = fixture.cli({"config", "list", "--show-token"});
    FMT_CHECK_EQ(with_token.exit_code, 0);
    FMT_CHECK(has(with_token, "HTTP 请求头：X-FMT-Token: "));

    // ---- 跨桶：按名字够不到别的桶，按 file_id 才够得到（要确认）----
    FMT_CHECK_EQ(fixture.cli({"bucket", "create", "other"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "other"}).exit_code, 0);

    const CliRun name_not_here = fixture.cli({"file", "delete", "first.txt"});
    FMT_CHECK_EQ(name_not_here.exit_code, 3);
    FMT_CHECK(has(name_not_here, "FMT-002"));
    FMT_CHECK(has(name_not_here, "work"));  // 说清同名的那个在哪个桶

    const CliRun cross_refused = fixture.cli({"file", "delete", first_id});
    FMT_CHECK_EQ(cross_refused.exit_code, 2);
    FMT_CHECK(has(cross_refused, "FMT-016"));
    FMT_CHECK(has(cross_refused, "work"));  // 说清是哪个桶

    // 拒绝之后文件确实还在：回 work 一看便知
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "work"}).exit_code, 0);
    const CliRun still_there = fixture.cli({"file", "get", "first.txt"});
    FMT_CHECK_EQ(still_there.exit_code, 0);

    // 带 --yes：这次真删
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "other"}).exit_code, 0);
    const CliRun cross_forced = fixture.cli({"file", "delete", first_id, "--yes"});
    FMT_CHECK_EQ(cross_forced.exit_code, 0);
    FMT_CHECK(has(cross_forced, "Bucket：work"));  // 成功消息带归属

    // ---- 永久删除：先拒绝，再执行 ----
    const CliRun purge_refused = fixture.cli({"trash", "delete", first_id});
    FMT_CHECK_EQ(purge_refused.exit_code, 2);
    FMT_CHECK(has(purge_refused, "FMT-016"));
    FMT_CHECK(has(purge_refused, "不可恢复"));

    const CliRun purged = fixture.cli({"trash", "delete", first_id, "--yes"});
    FMT_CHECK_EQ(purged.exit_code, 0);
    FMT_CHECK(has(purged, "已永久删除"));

    // ---- 删桶：空桶不打扰，非空桶要 --yes ----
    const CliRun empty_delete = fixture.cli({"bucket", "delete", "other"});
    FMT_CHECK_EQ(empty_delete.exit_code, 0);
    FMT_CHECK(has(empty_delete, "只能整体恢复"));  // 提醒照旧给出

    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "work"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("second.txt"))}).exit_code,
                 0);

    const CliRun bucket_refused = fixture.cli({"bucket", "delete", "work"});
    FMT_CHECK_EQ(bucket_refused.exit_code, 2);
    FMT_CHECK(has(bucket_refused, "FMT-016"));
    FMT_CHECK(has(bucket_refused, "只能整体恢复"));

    const CliRun bucket_deleted = fixture.cli({"bucket", "delete", "work", "--yes"});
    FMT_CHECK_EQ(bucket_deleted.exit_code, 0);

    const CliRun bucket_trash = fixture.cli({"trash", "list"});
    FMT_CHECK(has(bucket_trash, "[桶]"));
    FMT_CHECK(has(bucket_trash, "work"));
}

// 用户报的场景（顺序就是用户敲的顺序）：
//   桶 test 里有 a1.jpg 与 a3.jpg → 删掉 a3.jpg（文件级）→ 删掉整个桶 test
//   → 单独回退 a3.jpg → 再回退那个桶。
// 关键在于：删桶**之后**回退文件，会把 repository/<用户>/test/ 这个空目录建出来
//（回退要把文件放回桶里），于是最后一步的目标 Bucket「已存在」。以前它是
//「FMT-401 回退失败：Bucket 已存在」——回收站里的 a1.jpg 明明还在，却拿不回来。
// 现在：先说要合并几个文件、要一次确认（--yes），然后把文件合并进那个已存在的桶，
// 不覆盖、不改名；已经回退出来的 a3.jpg 一个字节都不动。
FMT_TEST(CliE2e, 桶已存在时回退是合并而不是回退失败) {
    E2eFixture fixture;

    FMT_CHECK_EQ(fixture.cli({"bucket", "create", "test"}).exit_code, 0);
    FMT_CHECK_EQ(
        fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt")), "a1.jpg"})
            .exit_code,
        0);
    FMT_CHECK_EQ(
        fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("second.txt")), "a3.jpg"})
            .exit_code,
        0);

    // ① 先删文件级的那一张（数据进 trash/<用户>/.files/），桶目录里就只剩 a1.jpg
    FMT_CHECK_EQ(fixture.cli({"file", "delete", "a3.jpg"}).exit_code, 0);

    // ② 再删整个桶（有内容要 --yes）
    FMT_CHECK_EQ(fixture.cli({"bucket", "delete", "test"}).exit_code, 2);  // 先拒一次
    FMT_CHECK_EQ(fixture.cli({"bucket", "delete", "test", "--yes"}).exit_code, 0);

    // ③ 回退 a3.jpg：它只是被单独删过（trash_reason="file"），所以能单独回退；
    //    这一步会把 repository/<用户>/test/ 建出来——正是「目标 Bucket 已存在」的来源
    const CliRun restore_a3 = fixture.cli({"trash", "restore", "a3.jpg"});
    FMT_CHECK_EQ(restore_a3.exit_code, 0);
    FMT_CHECK(has(restore_a3, "文件已回退"));
    FMT_CHECK_EQ(fixture.cli({"file", "get", "a3.jpg"}).exit_code, 0);

    // ④ 回退那个桶：目标已存在 -> 不是「回退失败」，而是**合并**，先要一次确认。
    //    CLI 打印的预检消息会说清「搬几个文件进去」（这里只有回收站里的 a1.jpg 一个）。
    const CliRun refused = fixture.cli({"trash", "restore", "test"});
    FMT_CHECK_EQ(refused.exit_code, 2);
    FMT_CHECK(has(refused, "FMT-016"));
    FMT_CHECK(has(refused, "合并"));
    FMT_CHECK(has(refused, "1 个文件"));
    FMT_CHECK(has(refused, "不覆盖"));
    // 被拒绝之后**一个文件都没搬**：桶目录里只有先前回退出来的 a3.jpg，
    // 回收站里的 a1.jpg 还躺在原处（合并要等用户确认）。
    {
        std::error_code code;
        std::size_t a1 = 0;
        const std::filesystem::path bucket = fixture.root() / "repository" / "user" / "test";
        for (const auto& item : std::filesystem::recursive_directory_iterator(bucket, code)) {
            std::error_code type_code;
            if (item.is_regular_file(type_code) &&
                item.path().filename() == L"a1.jpg") {
                ++a1;
            }
        }
        FMT_CHECK_EQ(a1, std::size_t{0});
    }

    // ⑤ 带 --yes：真的合并进去，消息如实说「已合并」
    const CliRun merged = fixture.cli({"trash", "restore", "test", "--yes"});
    FMT_CHECK_EQ(merged.exit_code, 0);
    FMT_CHECK(has(merged, "已合并"));

    // 两边的文件都在同一个桶里：合并进来的 a1.jpg + 本来就在的 a3.jpg
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "test"}).exit_code, 0);
    const CliRun listed = fixture.cli({"file", "list"});
    FMT_CHECK_EQ(listed.exit_code, 0);
    FMT_CHECK(has(listed, "a1.jpg"));
    FMT_CHECK(has(listed, "a3.jpg"));

    // 两个文件都能读回内容（合并是搬文件，不是只改记录）
    FMT_CHECK(has(fixture.cli({"file", "get", "a1.jpg"}), "a1.jpg"));
    FMT_CHECK(has(fixture.cli({"file", "get", "a3.jpg"}), "a3.jpg"));

    // 回收站里那条已经摘掉（合并完就不该再躺着）
    const CliRun trash = fixture.cli({"trash", "list"});
    FMT_CHECK_EQ(trash.exit_code, 0);
    FMT_CHECK(has(trash, "共 0 项"));
}

FMT_TEST(CliE2e, 分享能按文件名创建并打印完整链接) {
    E2eFixture fixture;

    FMT_CHECK_EQ(fixture.cli({"bucket", "create", "alpha"}).exit_code, 0);
    FMT_CHECK_EQ(
        fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt"))}).exit_code, 0);
    const CliRun detail = fixture.cli({"file", "get", "first.txt"});
    FMT_CHECK_EQ(detail.exit_code, 0);
    const std::string file_id = grab_file_id(detail);
    FMT_CHECK(!file_id.empty());

    // 按**文件名**创建分享（本次改动（未提交）：与 file get / file delete 同一套规则）
    const CliRun created = fixture.cli({"share", "create", "first.txt"});
    FMT_CHECK_EQ(created.exit_code, 0);
    FMT_CHECK(has(created, "share_id："));
    FMT_CHECK(has(created, file_id));  // 回显它挂的是哪个文件
    // 完整分享链接：无论 HTTP 服务有没有开都给（地址来自 config/server.json）
    const std::string share_id = grab_share_id(created);
    FMT_CHECK_EQ(share_id.size(), std::size_t{12});
    FMT_CHECK(has(created, "分享链接：http://localhost:4122/api/share/" + share_id + "/download"));

    // 按 file_id 一样能创建；share get / share list 也带链接
    const CliRun by_id = fixture.cli({"share", "create", file_id});
    FMT_CHECK_EQ(by_id.exit_code, 0);

    const CliRun viewed = fixture.cli({"share", "get", share_id});
    FMT_CHECK_EQ(viewed.exit_code, 0);
    FMT_CHECK(has(viewed, "分享链接：http://localhost:4122/api/share/" + share_id + "/download"));
    // share get 把「全部信息」都摊开（本次改动（未提交））：文件 / Bucket / 次数 / 到期 / 状态
    FMT_CHECK(has(viewed, "文件：first.txt（file_id " + file_id + "）"));
    FMT_CHECK(has(viewed, "Bucket：alpha"));
    FMT_CHECK(has(viewed, "下载次数：0/20"));
    FMT_CHECK(has(viewed, "到期："));
    FMT_CHECK(has(viewed, "状态：可用"));

    const CliRun listed = fixture.cli({"share", "list", "first.txt"});
    FMT_CHECK_EQ(listed.exit_code, 0);
    FMT_CHECK(has(listed, "共 2 条分享"));
    // 列表**不打印分享链接**（用户要求：一条一行扫状态；链接用 share get 看）
    FMT_CHECK(!has(listed, "api/share/"));

    // **不带参数**也列得出来（本次改动（未提交）），而且注明是「全部文件」
    const CliRun listed_all = fixture.cli({"share", "list"});
    FMT_CHECK_EQ(listed_all.exit_code, 0);
    FMT_CHECK(has(listed_all, "共 2 条分享（全部文件"));
    FMT_CHECK(has(listed_all, share_id));
    FMT_CHECK(has(listed_all, "first.txt（当前桶）"));  // 当前桶的文件标出来
    FMT_CHECK(has(listed_all, "下载次数已满") == false);  // 还没下过，不该有「已满」
    FMT_CHECK(!has(listed_all, "api/share/"));           // 列全部时也不打印链接

    // 把次数下满：`share list` 必须**直接标出「下载次数已满」**（用户要求）
    for (int i = 0; i < fmt::kDefaultShareDownloads; ++i) {
        FMT_CHECK_EQ(fixture.cli({"share", "download", share_id}).exit_code, 0);
    }
    const CliRun exhausted = fixture.cli({"share", "list"});
    FMT_CHECK_EQ(exhausted.exit_code, 0);
    FMT_CHECK(has(exhausted, "20/20"));
    FMT_CHECK(has(exhausted, "下载次数已满"));
    FMT_CHECK(has(exhausted, "下载次数已用尽"));  // 状态列
    // 用满之后 share get 也要说清楚；链接照给（「查看全部信息」），但标注它已经不可下载
    const CliRun spent = fixture.cli({"share", "get", share_id});
    FMT_CHECK_EQ(spent.exit_code, 0);
    FMT_CHECK(has(spent, "下载次数：20/20（下载次数已满）"));
    FMT_CHECK(has(spent, "状态：下载次数已用尽"));
    FMT_CHECK(has(spent, "分享链接：http://localhost:4122/api/share/" + share_id +
                             "/download（当前不可下载，链接已失效）"));

    // 名字不存在 → 与 file get 一致：FMT-002
    const CliRun absent = fixture.cli({"share", "create", "nope.txt"});
    FMT_CHECK_EQ(absent.exit_code, 3);
    FMT_CHECK(has(absent, "FMT-002"));
}

FMT_TEST(CliE2e, 交互式确认答n不删答y才删) {
    E2eFixture fixture;

    FMT_CHECK_EQ(fixture.cli({"bucket", "create", "alpha"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.cli({"bucket", "create", "beta"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "alpha"}).exit_code, 0);
    FMT_CHECK_EQ(
        fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt"))}).exit_code, 0);

    // 把 file_id 捞出来给交互式命令用：切到 beta 之后**按名字**已经够不到 alpha 里的文件
    // （本次改动（未提交）：名字的命名空间是 Bucket），跨桶删除只能用 file_id（并且要确认）。
    const CliRun one = fixture.cli({"file", "get", "first.txt"});
    FMT_CHECK_EQ(one.exit_code, 0);
    const std::string first_id = grab_file_id(one);
    FMT_CHECK(!first_id.empty());

    // 交互式：答 n —— 必须「已取消」且**文件还在**
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "beta"}).exit_code, 0);
    const CliRun declined = run_interactive_until_output(
        fixture, "file delete " + first_id + "\nn\nbucket use alpha\nfile list\nexit\n");
    FMT_CHECK(has(declined, "确认执行？"));
    FMT_CHECK(has(declined, "已取消"));
    FMT_CHECK(has(declined, "first.txt"));  // 文件仍在 alpha 的列表里

    // 交互式：答 y —— 这次真的删掉
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "beta"}).exit_code, 0);
    const CliRun accepted = run_interactive_until_output(
        fixture, "file delete " + first_id + "\ny\ntrash list\nexit\n");
    FMT_CHECK(has(accepted, "确认执行？"));
    FMT_CHECK(!has(accepted, "已取消"));
    FMT_CHECK(has(accepted, "[文件]"));
    FMT_CHECK(has(accepted, "first.txt"));
}
