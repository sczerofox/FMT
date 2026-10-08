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

// 一个隔离的端到端环境：唯一管道名 + 独立数据根（fmt.exe 就放在里面，因为
// 「数据根 = CLI 所在目录」）+ 进程内服务。
class E2eFixture {
public:
    E2eFixture() {
        static std::atomic<unsigned> counter{0};
        pipe_ = L"\\\\.\\pipe\\fmt-e2e-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(counter.fetch_add(1));
        SetEnvironmentVariableW(L"FMT_PIPE", pipe_.c_str());

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
        // CLI 侧的 connect_waiting 会重试，所以这里只需要给 accept 循环一点时间起来
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    ~E2eFixture() {
        runtime_->request_stop();
        if (runner_.joinable()) {
            runner_.join();
        }
        SetEnvironmentVariableW(L"FMT_PIPE", nullptr);
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

    // 同内容再传一次：FMT-304
    const CliRun duplicate =
        fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt"))});
    FMT_CHECK_EQ(duplicate.exit_code, 4);
    FMT_CHECK(has(duplicate, "FMT-304"));

    // 同名不同内容：FMT-105
    const CliRun conflict =
        fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("second.txt")), "first.txt"});
    FMT_CHECK_EQ(conflict.exit_code, 4);
    FMT_CHECK(has(conflict, "FMT-105"));

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

    const CliRun missing = fixture.cli({"file", "get", "nope.txt"});
    FMT_CHECK_EQ(missing.exit_code, 3);
    FMT_CHECK(has(missing, "FMT-002"));

    // ---- 软删除 -> 回收站 -> 回退（文件级读侧）----
    const CliRun removed = fixture.cli({"file", "delete", "first.txt"});
    FMT_CHECK_EQ(removed.exit_code, 0);
    FMT_CHECK(has(removed, "回收站"));

    const CliRun trash = fixture.cli({"trash", "list"});
    FMT_CHECK_EQ(trash.exit_code, 0);
    FMT_CHECK(has(trash, "[文件]"));
    FMT_CHECK(has(trash, "first.txt"));

    const CliRun trash_get = fixture.cli({"trash", "get", first_id});
    FMT_CHECK_EQ(trash_get.exit_code, 0);
    FMT_CHECK(has(trash_get, "回收站路径"));

    const CliRun restored = fixture.cli({"trash", "restore", first_id});
    FMT_CHECK_EQ(restored.exit_code, 0);
    FMT_CHECK(has(restored, "已回退"));

    // ---- 跨桶删除：没 --yes 要拒绝，而且**不能真删** ----
    FMT_CHECK_EQ(fixture.cli({"bucket", "create", "other"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "other"}).exit_code, 0);

    const CliRun cross_refused = fixture.cli({"file", "delete", "first.txt"});
    FMT_CHECK_EQ(cross_refused.exit_code, 2);
    FMT_CHECK(has(cross_refused, "FMT-016"));
    FMT_CHECK(has(cross_refused, "work"));  // 说清是哪个桶

    // 拒绝之后文件确实还在：回 work 一看便知
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "work"}).exit_code, 0);
    const CliRun still_there = fixture.cli({"file", "get", "first.txt"});
    FMT_CHECK_EQ(still_there.exit_code, 0);

    // 带 --yes：这次真删
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "other"}).exit_code, 0);
    const CliRun cross_forced = fixture.cli({"file", "delete", "first.txt", "--yes"});
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

FMT_TEST(CliE2e, 交互式确认答n不删答y才删) {
    E2eFixture fixture;

    FMT_CHECK_EQ(fixture.cli({"bucket", "create", "alpha"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.cli({"bucket", "create", "beta"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "alpha"}).exit_code, 0);
    FMT_CHECK_EQ(
        fixture.cli({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt"))}).exit_code, 0);

    // 切到 beta：这样在 alpha 里的那个文件属于别的桶 -> 删除要确认
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "beta"}).exit_code, 0);

    // 交互式：答 n —— 必须「已取消」且**文件还在**
    const CliRun declined = fixture.cli({}, "file delete first.txt\nn\nbucket use alpha\nfile list\nexit\n");
    FMT_CHECK(has(declined, "确认执行？"));
    FMT_CHECK(has(declined, "已取消"));
    FMT_CHECK(has(declined, "first.txt"));  // 文件仍在 alpha 的列表里

    // 交互式：答 y —— 这次真的删掉
    FMT_CHECK_EQ(fixture.cli({"bucket", "use", "beta"}).exit_code, 0);
    const CliRun accepted = fixture.cli({}, "file delete first.txt\ny\ntrash list\nexit\n");
    FMT_CHECK(has(accepted, "确认执行？"));
    FMT_CHECK(!has(accepted, "已取消"));
    FMT_CHECK(has(accepted, "[文件]"));
    FMT_CHECK(has(accepted, "first.txt"));
}
