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
#include "fmt/user/user.hpp"
#include "fmt/user/user_service.hpp"
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
        // 子进程用的是 FMT_MUTEX 指定的名字（fixture 每实例唯一）；探针必须查**同一个**，
        // 否则会去查 Local\FMT.CLI.v1 然后因为用户开着窗口而误报。
        wchar_t override_name[256] = {};
        const DWORD override_len =
            GetEnvironmentVariableW(L"FMT_MUTEX", override_name, 256);
        std::wstring probe_name =
            override_len > 0 ? std::wstring(L"Local\\") + override_name : std::wstring(L"Local\\FMT.CLI.v1");
        HANDLE probe = CreateMutexW(nullptr, FALSE, probe_name.c_str());
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

// 出现次数：用来区分「登录那一次问了密码」与「命令自己又问了一遍」。
std::size_t count_of(const CliRun& run, const std::string& needle) {
    std::size_t total = 0;
    for (std::size_t at = run.output.find(needle); at != std::string::npos;
         at = run.output.find(needle, at + needle.size())) {
        ++total;
    }
    return total;
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
        // 单实例互斥体也换一个名字：跑测试时用户可能正开着 FMT 窗口，
        // 无参数交互式子进程撞上那个互斥体会**静默退出**，用例就变成"没有输出"。
        // 纯测试用开关，正常使用不设它。
        const std::wstring mutex_name = L"fmt-e2e-cli-" + std::to_wstring(GetCurrentProcessId()) +
                                        L"-" + std::to_wstring(counter.fetch_add(1));
        SetEnvironmentVariableW(L"FMT_MUTEX", mutex_name.c_str());

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

        // 账号：2.0.0 起 initialize_root **不再自动建账号**（用户要求：第一次启动由
        // `user create` 自己建）。E2E 测的是「有账号之后」的链路，所以这里先用
        // UserService 建一个——和 CLI 的 user create 走同一段代码。
        // 服务是**每请求现读** data/user.json 验 token 的（UserStore::find_by_token），
        // 所以在这里直接建好，起服务之后立刻就能用。
        {
            const auto initialized = fmt::initialize_root(deploy_);
            FMT_CHECK(fmt::ok(initialized));
            const fmt::PathManager paths{deploy_};
            fmt::Config config = std::get<fmt::Config>(fmt::load_config(paths));
            fmt::user::UserService users(paths, config);
            const auto created =
                users.create("user", fmt::password_digest("e2e-secret", fmt::directory_salt("user")));
            FMT_CHECK(fmt::ok(created));
        }

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
        SetEnvironmentVariableW(L"FMT_MUTEX", nullptr);
    }

    E2eFixture(const E2eFixture&) = delete;
    E2eFixture& operator=(const E2eFixture&) = delete;

    CliRun cli(const std::vector<std::string>& args, const std::string& stdin_text = {}) const {
        return run_cli(deploy_ / L"fmt.exe", args, stdin_text);
    }

    // 业务命令必须先登录（用户要求）：一次性命令要在**同一次调用**里带 `-u/-p`，
    // 所以这里的口令走 stdin（非控制台时 read_password_masked 按行读）。
    //
    // 登录之后的服务端认证并没有变：管道请求仍然由服务每请求验 token
    //（`data/user.json` 里那个）——所以 Java 侧照旧能用 token 直连（临时备用钥匙）。
    CliRun business(const std::vector<std::string>& args,
                    const std::string& stdin_text = {}) const {
        return business_with_password(kTestPassword, args, stdin_text);
    }

    // 改过口令之后要用新口令登录（口令只能改一次，测试里也是）。
    CliRun business_with_password(const std::string& login_password,
                                  const std::vector<std::string>& args,
                                  const std::string& stdin_text = {}) const {
        std::vector<std::string> full;
        full.reserve(args.size() + 3);
        full.push_back("-u");
        full.push_back(kTestUser);
        full.push_back("-p");
        for (const std::string& item : args) {
            full.push_back(item);
        }
        std::string feed = login_password;
        feed += "\n";
        feed += stdin_text;
        return run_cli(deploy_ / L"fmt.exe", full, feed);
    }

    std::filesystem::path source(const std::string& name) const { return temp_.path() / name; }
    const std::filesystem::path& root() const { return deploy_; }
    static constexpr const char* kTestUser = "user";
    static constexpr const char* kTestPassword = "e2e-secret";

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
    // 交互式进程是第一行就登录（`-u/-p`）：登录态只在本次进程内存里，
    // 所以文件命令必须落在同一个进程里才能执行——正是这次要验证的门禁行为。
    const std::string feed = std::string(E2eFixture::kTestPassword) + "\n" + stdin_text;
    const std::vector<std::string> args = {"-u", E2eFixture::kTestUser, "-p"};
    CliRun run = fixture.cli(args, feed);
    for (int attempt = 0; attempt < 2 && run.output.empty(); ++attempt) {
        std::fprintf(stderr, "[e2e] 交互式子进程这次没有任何输出（退出码 %d），重跑一次\n",
                     run.exit_code);
        run = fixture.cli(args, feed);
    }
    return run;
}

// 交互式：**不用启动参数**，进窗口后自己敲 `login`（真机反馈的用法）。
CliRun run_interactive_via_login_command(const E2eFixture& fixture,
                                         const std::string& stdin_text) {
    CliRun run = fixture.cli({}, stdin_text);
    for (int attempt = 0; attempt < 2 && run.output.empty(); ++attempt) {
        std::fprintf(stderr, "[e2e] 交互式子进程这次没有任何输出（退出码 %d），重跑一次\n",
                     run.exit_code);
        run = fixture.cli({}, stdin_text);
    }
    return run;
}

}  // namespace

FMT_TEST(CliE2e, user命令打出账号字段而不是只剩一句提示) {
    // 真机踩过：`user get` 只回了一句「账号信息（密码还可以修改一次）」，
    // 账号字段一个都没打出来——因为服务端把字段都返回了，而 CLI 的打印函数
    // 没有 user 分支，只走了兜底的 message。这条用例把版式钉住。
    //
    // 同时钉住「口令输入的回退路径」：stdin 被重定向（自动化）时
    // read_password_masked 必须退回按行读，不能跑去读真控制台——那样脚本会卡死。
    //
    // fixture 已经建好账号（见 E2eFixture 构造函数），口令是 e2e-secret。
    E2eFixture fixture;

    // 一个数据根一个账号：再 create 一律拒绝（绝不覆盖）。
    // **必须在问口令之前就拒绝**（真机反馈：以前先让人输两遍密码，最后才说"已经有账号"）。
    // 这里喂 4 行而命令只需要 2 行——如果它真去读了口令，输出里会出现"请输入密码"。
    // 走 business()：已有账号时 user create 也算业务操作，要先登录。
    const CliRun again = fixture.business({"user", "create", "other"}, "x\nx\ny\ny\n");
    FMT_CHECK(again.exit_code != 0);
    FMT_CHECK(has(again, "已经有账号"));
    FMT_CHECK(has(again, "user delete"));        // 告诉用户下一步该敲什么
    // 关键：`user create` 自己**没有**再问一遍口令。
    // business() 的 `-p` 登录会打印一次「请输入密码」，所以这里要求总数恰好为 1。
    FMT_CHECK_EQ(count_of(again, "请输入密码"), std::size_t{1});

    // 未登录 → 业务命令被拒（用户要求的门禁），并且提示要**照着能做**。
    const CliRun denied = fixture.cli({"file", "list"});
    FMT_CHECK(denied.exit_code != 0);
    FMT_CHECK(has(denied, "需要先登录"));
    FMT_CHECK(has(denied, "fmt -u"));            // 一次性命令：给命令行用法
    FMT_CHECK(!has(denied, "请重新打开窗口"));    // 窗口内就能登录，不该把人往外推

    // `service status` 不受门禁限制（要能靠它把机器救回来）。
    const CliRun status = fixture.cli({"service", "status"});
    FMT_CHECK(!has(status, "需要先登录"));

    // 窗口内登录（真机反馈的用法）：进了提示符再敲 login，登录后同一次会话里业务命令可用。
    // 曾经 `-u/-p` 只在启动参数里有效，窗口里敲 `fmt -u lazy -p` 会被门禁当成未知命令组。
    const CliRun in_prompt = run_interactive_via_login_command(
        fixture, "login\n" + std::string(E2eFixture::kTestPassword) +
                      "\nfile list\nuser get\nexit\n");
    FMT_CHECK(has(in_prompt, "登录成功"));
    FMT_CHECK(has(in_prompt, "密码"));            // 问了密码
    FMT_CHECK(!has(in_prompt, "需要先登录"));     // 登录之后没再拦
    // 登录要**更新时间戳**（用户要求）：窗口内 user get 不该再说「还没登录过」。
    FMT_CHECK(has(in_prompt, "最后登录："));
    FMT_CHECK(!has(in_prompt, "最后登录：（还没登录过）"));
    // 而且登录之后 `user get` 要能给出 token。
    FMT_CHECK(has(in_prompt, "访问 token："));

    // 窗口内 logout：登出之后业务命令又要登录
    const CliRun after_logout = run_interactive_via_login_command(
        fixture, "login\n" + std::string(E2eFixture::kTestPassword) +
                      "\nlogout\nfile list\nexit\n");
    FMT_CHECK(has(after_logout, "已退出登录"));
    FMT_CHECK(has(after_logout, "需要先登录"));   // 门禁回来了

    // `user get` **要登录**（用户要求）：未登录必须被拒。
    const CliRun denied_get = fixture.cli({"user", "get"});
    FMT_CHECK(denied_get.exit_code != 0);
    FMT_CHECK(has(denied_get, "需要先登录"));

    // 关键回归：登录之后 `user get` 必须把字段与 token 都列出来，不能只剩一句提示。
    const CliRun got = fixture.business({"user", "get"});
    if (got.exit_code != 0) {
        std::fprintf(stderr, "[e2e] user get 输出：\n%s\n", got.output.c_str());
    }
    FMT_CHECK_EQ(got.exit_code, 0);
    FMT_CHECK(has(got, "用户名：user"));
    FMT_CHECK(has(got, "用户 ID："));
    FMT_CHECK(has(got, "当前 Bucket："));
    FMT_CHECK(has(got, "创建时间："));
    FMT_CHECK(has(got, "密码状态："));
    FMT_CHECK(has(got, "还可以修改一次"));
    FMT_CHECK(has(got, "访问 token："));      // 用户要求：user get 要能看当前 token
    FMT_CHECK(has(got, "X-FMT-Token:"));
    FMT_CHECK(!has(got, "password_hash"));   // 口令哈希/盐仍然一个都不打
    FMT_CHECK(!has(got, "password_salt"));

    // `user delete` **既不需要登录、也不需要密码**（用户要求：忘记密码时这是唯一出路）。
    // 防误触靠的是「逐字输入 delete」。
    const CliRun refused = fixture.cli({"user", "delete"}, "no\n");
    FMT_CHECK_EQ(refused.exit_code, 0);
    FMT_CHECK(has(refused, "已取消"));
    FMT_CHECK(fmt::file_exists(fixture.root() / "data" / "user.json"));  // 数据还在

    // 登录也不该是前置条件：这一条不带 -u/-p 也必须能走到确认提示。
    FMT_CHECK(!has(refused, "需要先登录"));
    FMT_CHECK(has(refused, "逐字输入 delete"));

    // token 换发：**要登录**（用户要求：改密码、换 token 都得先登录）。
    const CliRun renew_denied = fixture.cli({"user", "token", "renew"});
    FMT_CHECK(renew_denied.exit_code != 0);
    FMT_CHECK(has(renew_denied, "需要先登录"));

    // 登录之后换发：新值和有效期都必须打出来
    // ——否则"换发成功"等于什么都没发生（用户拿不到新 token）。
    const CliRun renewed = fixture.business({"user", "token", "renew"});
    FMT_CHECK_EQ(renewed.exit_code, 0);
    FMT_CHECK(has(renewed, "访问 token："));
    FMT_CHECK(has(renewed, "token 有效期："));
    FMT_CHECK(has(renewed, "旧 token 立即失效"));
    // 登录时已经算过摘要，换 token **不该再问第二遍密码**。
    FMT_CHECK_EQ(count_of(renewed, "请输入密码"), std::size_t{1});

    // 改密：**不需要先登录**（自己会问当前密码），用户明确要求这条不能逼人登录。
    // 只能改一次：第一次成功，第二次被拒并提示删除用户重建。
    const CliRun changed =
        fixture.cli({"user", "update", "password"}, "e2e-secret\nnew-pw\nnew-pw\n");
    FMT_CHECK_EQ(changed.exit_code, 0);
    FMT_CHECK(has(changed, "密码已更新"));

    const CliRun second = fixture.business_with_password(
        "new-pw", {"user", "update", "password"}, "new-pw\nthird-pw\nthird-pw\n");
    FMT_CHECK(second.exit_code != 0);
    FMT_CHECK(has(second, "只能修改一次") || has(second, "不能再改"));

    // 改完之后 `user get`（**要登录**，用新口令）如实报告「不能再改」。
    const CliRun after = fixture.business_with_password("new-pw", {"user", "get"});
    FMT_CHECK_EQ(after.exit_code, 0);
    FMT_CHECK(has(after, "已经修改过一次"));

    // 真删：逐字 delete 才动手，删完立刻重建目录骨架（同一进程里能接着 user create）。
    const CliRun deleted = fixture.cli({"user", "delete"}, "delete\n");
    FMT_CHECK_EQ(deleted.exit_code, 0);
    FMT_CHECK(has(deleted, "已永久删除"));
    FMT_CHECK(has(deleted, "目录结构已重建"));
    FMT_CHECK(has(deleted, "user create"));
    // 账号没了：user.json 只剩版本号；桶目录也清空了。
    const auto remaining = fmt::read_json_file(fixture.root() / "data" / "user.json");
    FMT_CHECK(fmt::ok(remaining));
    if (fmt::ok(remaining)) {
        FMT_CHECK(!std::get<nlohmann::json>(remaining).contains("username"));
    }
    FMT_CHECK(fmt::file_exists(fixture.root() / "data" / "file.json"));   // 骨架重建了
    FMT_CHECK(!fmt::file_exists(fixture.root() / "repository" / "user"));  // 业务数据没了
}

FMT_TEST(CliE2e, 核心链路走真实exe与真实管道) {
    E2eFixture fixture;

    // ---- 不需要服务的本地命令 ----
    const CliRun version = fixture.cli({"version"});
    FMT_CHECK_EQ(version.exit_code, 0);
    FMT_CHECK(has(version, "File Manager Tool"));

    // ---- Bucket ----
    const CliRun create = fixture.business({"bucket", "create", "WORK"});
    FMT_CHECK_EQ(create.exit_code, 0);
    FMT_CHECK(has(create, "work"));      // 统一小写
    FMT_CHECK(has(create, "小写"));       // 并且提示了用户

    const CliRun buckets = fixture.business({"bucket", "list"});
    FMT_CHECK_EQ(buckets.exit_code, 0);
    FMT_CHECK(has(buckets, "work"));

    // ---- 上传（本地路径）----
    const CliRun upload = fixture.business({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt"))});
    FMT_CHECK_EQ(upload.exit_code, 0);
    FMT_CHECK(has(upload, "first.txt"));
    const std::string first_id = grab_file_id(upload);
    FMT_CHECK(!first_id.empty());

    // 同内容 + **不同文件名**：同一个桶里同一份内容只留一份 → FMT-304「文件已存在」，
    // **不提示改文件名**（用户的四条口径之一）
    const CliRun same_content =
        fixture.business({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt")), "first-copy.txt"});
    FMT_CHECK_EQ(same_content.exit_code, 4);
    FMT_CHECK(has(same_content, "FMT-304"));
    FMT_CHECK(has(same_content, "文件已存在"));
    FMT_CHECK(has(same_content, "first.txt"));        // 说清是哪一份
    FMT_CHECK(!has(same_content, "修改文件名"));       // 但不要求改名

    // 同内容 + 同文件名：同样 FMT-304「文件已存在」，同样不提示改文件名
    const CliRun duplicate =
        fixture.business({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt")), "first.txt"});
    FMT_CHECK_EQ(duplicate.exit_code, 4);
    FMT_CHECK(has(duplicate, "FMT-304"));
    FMT_CHECK(has(duplicate, "文件已存在"));
    FMT_CHECK(!has(duplicate, "修改文件名"));

    // 不同内容 + 同文件名：FMT-105「文件名重复」，这次**要提示改文件名**
    const CliRun conflict =
        fixture.business({"file", "upload", fmt::path_to_utf8(fixture.source("second.txt")), "first.txt"});
    FMT_CHECK_EQ(conflict.exit_code, 4);
    FMT_CHECK(has(conflict, "FMT-105"));
    FMT_CHECK(has(conflict, "文件名重复"));
    FMT_CHECK(has(conflict, "修改文件名"));

    // ---- 查询 ----
    const CliRun list = fixture.business({"file", "list"});
    FMT_CHECK_EQ(list.exit_code, 0);
    FMT_CHECK(has(list, "first.txt"));

    const CliRun by_name = fixture.business({"file", "get", "first.txt"});
    FMT_CHECK_EQ(by_name.exit_code, 0);
    FMT_CHECK(has(by_name, first_id));

    const CliRun by_id = fixture.business({"file", "get", first_id});
    FMT_CHECK_EQ(by_id.exit_code, 0);
    FMT_CHECK(has(by_id, "first.txt"));
    // 预览 / 下载链接（本次改动（未提交））：完整 http:// 地址，HTTP 服务开着也照样给
    FMT_CHECK(has(by_id, "预览链接：http://localhost:4122/api/file/" + first_id + "/preview"));
    FMT_CHECK(has(by_id, "下载链接：http://localhost:4122/api/file/" + first_id + "/download"));

    const CliRun missing = fixture.business({"file", "get", "nope.txt"});
    FMT_CHECK_EQ(missing.exit_code, 3);
    FMT_CHECK(has(missing, "FMT-002"));

    // ---- config list：config.json 的字段 + 网络服务三项（本次改动（未提交））----
    // 只读！`config set server_*` 会触发「改完自动重启服务」（CLI 侧提权重装），
    // 在端到端用例里那会去动**真实的 Windows 服务**（提权弹窗也挡不住断言），
    // 所以这里只断言 `config list` 的输出格式；写入路径由 Service.* 单测覆盖。
    const CliRun config = fixture.business({"config", "list"});
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
    const CliRun with_token = fixture.business({"config", "list", "--show-token"});
    FMT_CHECK_EQ(with_token.exit_code, 0);
    FMT_CHECK(has(with_token, "HTTP 请求头：X-FMT-Token: "));

    // ---- 跨桶：按名字够不到别的桶，按 file_id 才够得到（要确认）----
    FMT_CHECK_EQ(fixture.business({"bucket", "create", "other"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.business({"bucket", "use", "other"}).exit_code, 0);

    const CliRun name_not_here = fixture.business({"file", "delete", "first.txt"});
    FMT_CHECK_EQ(name_not_here.exit_code, 3);
    FMT_CHECK(has(name_not_here, "FMT-002"));
    FMT_CHECK(has(name_not_here, "work"));  // 说清同名的那个在哪个桶

    const CliRun cross_refused = fixture.business({"file", "delete", first_id});
    FMT_CHECK_EQ(cross_refused.exit_code, 2);
    FMT_CHECK(has(cross_refused, "FMT-016"));
    FMT_CHECK(has(cross_refused, "work"));  // 说清是哪个桶

    // 拒绝之后文件确实还在：回 work 一看便知
    FMT_CHECK_EQ(fixture.business({"bucket", "use", "work"}).exit_code, 0);
    const CliRun still_there = fixture.business({"file", "get", "first.txt"});
    FMT_CHECK_EQ(still_there.exit_code, 0);

    // 带 --yes：这次真删
    FMT_CHECK_EQ(fixture.business({"bucket", "use", "other"}).exit_code, 0);
    const CliRun cross_forced = fixture.business({"file", "delete", first_id, "--yes"});
    FMT_CHECK_EQ(cross_forced.exit_code, 0);
    FMT_CHECK(has(cross_forced, "Bucket：work"));  // 成功消息带归属

    // ---- 永久删除：先拒绝，再执行 ----
    const CliRun purge_refused = fixture.business({"trash", "delete", first_id});
    FMT_CHECK_EQ(purge_refused.exit_code, 2);
    FMT_CHECK(has(purge_refused, "FMT-016"));
    FMT_CHECK(has(purge_refused, "不可恢复"));

    const CliRun purged = fixture.business({"trash", "delete", first_id, "--yes"});
    FMT_CHECK_EQ(purged.exit_code, 0);
    FMT_CHECK(has(purged, "已永久删除"));

    // ---- 删桶：空桶不打扰，非空桶要 --yes ----
    const CliRun empty_delete = fixture.business({"bucket", "delete", "other"});
    FMT_CHECK_EQ(empty_delete.exit_code, 0);
    FMT_CHECK(has(empty_delete, "只能整体恢复"));  // 提醒照旧给出

    FMT_CHECK_EQ(fixture.business({"bucket", "use", "work"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.business({"file", "upload", fmt::path_to_utf8(fixture.source("second.txt"))}).exit_code,
                 0);

    const CliRun bucket_refused = fixture.business({"bucket", "delete", "work"});
    FMT_CHECK_EQ(bucket_refused.exit_code, 2);
    FMT_CHECK(has(bucket_refused, "FMT-016"));
    FMT_CHECK(has(bucket_refused, "只能整体恢复"));

    const CliRun bucket_deleted = fixture.business({"bucket", "delete", "work", "--yes"});
    FMT_CHECK_EQ(bucket_deleted.exit_code, 0);

    const CliRun bucket_trash = fixture.business({"trash", "list"});
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

    FMT_CHECK_EQ(fixture.business({"bucket", "create", "test"}).exit_code, 0);
    FMT_CHECK_EQ(
        fixture.business({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt")), "a1.jpg"})
            .exit_code,
        0);
    FMT_CHECK_EQ(
        fixture.business({"file", "upload", fmt::path_to_utf8(fixture.source("second.txt")), "a3.jpg"})
            .exit_code,
        0);

    // ① 先删文件级的那一张（数据进 trash/<用户>/.files/），桶目录里就只剩 a1.jpg
    FMT_CHECK_EQ(fixture.business({"file", "delete", "a3.jpg"}).exit_code, 0);

    // ② 再删整个桶（有内容要 --yes）
    FMT_CHECK_EQ(fixture.business({"bucket", "delete", "test"}).exit_code, 2);  // 先拒一次
    FMT_CHECK_EQ(fixture.business({"bucket", "delete", "test", "--yes"}).exit_code, 0);

    // ③ 回退 a3.jpg：它只是被单独删过（trash_reason="file"），所以能单独回退；
    //    这一步会把 repository/<用户>/test/ 建出来——正是「目标 Bucket 已存在」的来源
    const CliRun restore_a3 = fixture.business({"trash", "restore", "a3.jpg"});
    FMT_CHECK_EQ(restore_a3.exit_code, 0);
    FMT_CHECK(has(restore_a3, "文件已回退"));
    FMT_CHECK_EQ(fixture.business({"file", "get", "a3.jpg"}).exit_code, 0);

    // ④ 回退那个桶：目标已存在 -> 不是「回退失败」，而是**合并**，先要一次确认。
    //    CLI 打印的预检消息会说清「搬几个文件进去」（这里只有回收站里的 a1.jpg 一个）。
    const CliRun refused = fixture.business({"trash", "restore", "test"});
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
    const CliRun merged = fixture.business({"trash", "restore", "test", "--yes"});
    FMT_CHECK_EQ(merged.exit_code, 0);
    FMT_CHECK(has(merged, "已合并"));

    // 两边的文件都在同一个桶里：合并进来的 a1.jpg + 本来就在的 a3.jpg
    FMT_CHECK_EQ(fixture.business({"bucket", "use", "test"}).exit_code, 0);
    const CliRun listed = fixture.business({"file", "list"});
    FMT_CHECK_EQ(listed.exit_code, 0);
    FMT_CHECK(has(listed, "a1.jpg"));
    FMT_CHECK(has(listed, "a3.jpg"));

    // 两个文件都能读回内容（合并是搬文件，不是只改记录）
    FMT_CHECK(has(fixture.business({"file", "get", "a1.jpg"}), "a1.jpg"));
    FMT_CHECK(has(fixture.business({"file", "get", "a3.jpg"}), "a3.jpg"));

    // 回收站里那条已经摘掉（合并完就不该再躺着）
    const CliRun trash = fixture.business({"trash", "list"});
    FMT_CHECK_EQ(trash.exit_code, 0);
    FMT_CHECK(has(trash, "共 0 项"));
}

FMT_TEST(CliE2e, 分享能按文件名创建并打印完整链接) {
    E2eFixture fixture;

    FMT_CHECK_EQ(fixture.business({"bucket", "create", "alpha"}).exit_code, 0);
    FMT_CHECK_EQ(
        fixture.business({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt"))}).exit_code, 0);
    const CliRun detail = fixture.business({"file", "get", "first.txt"});
    FMT_CHECK_EQ(detail.exit_code, 0);
    const std::string file_id = grab_file_id(detail);
    FMT_CHECK(!file_id.empty());

    // 按**文件名**创建分享（本次改动（未提交）：与 file get / file delete 同一套规则）
    const CliRun created = fixture.business({"share", "create", "first.txt"});
    FMT_CHECK_EQ(created.exit_code, 0);
    FMT_CHECK(has(created, "share_id："));
    FMT_CHECK(has(created, file_id));  // 回显它挂的是哪个文件
    // 完整分享链接：无论 HTTP 服务有没有开都给（地址来自 config/server.json）
    const std::string share_id = grab_share_id(created);
    FMT_CHECK_EQ(share_id.size(), std::size_t{12});
    FMT_CHECK(has(created, "分享链接：http://localhost:4122/api/share/" + share_id + "/download"));

    // 按 file_id 一样能创建；share get / share list 也带链接
    const CliRun by_id = fixture.business({"share", "create", file_id});
    FMT_CHECK_EQ(by_id.exit_code, 0);

    const CliRun viewed = fixture.business({"share", "get", share_id});
    FMT_CHECK_EQ(viewed.exit_code, 0);
    FMT_CHECK(has(viewed, "分享链接：http://localhost:4122/api/share/" + share_id + "/download"));
    // share get 把「全部信息」都摊开（本次改动（未提交））：文件 / Bucket / 次数 / 到期 / 状态
    FMT_CHECK(has(viewed, "文件：first.txt（file_id " + file_id + "）"));
    FMT_CHECK(has(viewed, "Bucket：alpha"));
    FMT_CHECK(has(viewed, "下载次数：0/20"));
    FMT_CHECK(has(viewed, "到期："));
    FMT_CHECK(has(viewed, "状态：可用"));

    const CliRun listed = fixture.business({"share", "list", "first.txt"});
    FMT_CHECK_EQ(listed.exit_code, 0);
    FMT_CHECK(has(listed, "共 2 条分享"));
    // 列表**不打印分享链接**（用户要求：一条一行扫状态；链接用 share get 看）
    FMT_CHECK(!has(listed, "api/share/"));

    // **不带参数**也列得出来（本次改动（未提交）），而且注明是「全部文件」
    const CliRun listed_all = fixture.business({"share", "list"});
    FMT_CHECK_EQ(listed_all.exit_code, 0);
    FMT_CHECK(has(listed_all, "共 2 条分享（全部文件"));
    FMT_CHECK(has(listed_all, share_id));
    FMT_CHECK(has(listed_all, "first.txt（当前桶）"));  // 当前桶的文件标出来
    FMT_CHECK(has(listed_all, "下载次数已满") == false);  // 还没下过，不该有「已满」
    FMT_CHECK(!has(listed_all, "api/share/"));           // 列全部时也不打印链接

    // 把次数下满：`share list` 必须**直接标出「下载次数已满」**（用户要求）
    for (int i = 0; i < fmt::kDefaultShareDownloads; ++i) {
        FMT_CHECK_EQ(fixture.business({"share", "download", share_id}).exit_code, 0);
    }
    const CliRun exhausted = fixture.business({"share", "list"});
    FMT_CHECK_EQ(exhausted.exit_code, 0);
    FMT_CHECK(has(exhausted, "20/20"));
    FMT_CHECK(has(exhausted, "下载次数已满"));
    FMT_CHECK(has(exhausted, "下载次数已用尽"));  // 状态列
    // 用满之后 share get 也要说清楚；链接照给（「查看全部信息」），但标注它已经不可下载
    const CliRun spent = fixture.business({"share", "get", share_id});
    FMT_CHECK_EQ(spent.exit_code, 0);
    FMT_CHECK(has(spent, "下载次数：20/20（下载次数已满）"));
    FMT_CHECK(has(spent, "状态：下载次数已用尽"));
    FMT_CHECK(has(spent, "分享链接：http://localhost:4122/api/share/" + share_id +
                             "/download（当前不可下载，链接已失效）"));

    // 名字不存在 → 与 file get 一致：FMT-002
    const CliRun absent = fixture.business({"share", "create", "nope.txt"});
    FMT_CHECK_EQ(absent.exit_code, 3);
    FMT_CHECK(has(absent, "FMT-002"));
}

FMT_TEST(CliE2e, 交互式确认答n不删答y才删) {
    E2eFixture fixture;

    FMT_CHECK_EQ(fixture.business({"bucket", "create", "alpha"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.business({"bucket", "create", "beta"}).exit_code, 0);
    FMT_CHECK_EQ(fixture.business({"bucket", "use", "alpha"}).exit_code, 0);
    FMT_CHECK_EQ(
        fixture.business({"file", "upload", fmt::path_to_utf8(fixture.source("first.txt"))}).exit_code, 0);

    // 把 file_id 捞出来给交互式命令用：切到 beta 之后**按名字**已经够不到 alpha 里的文件
    // （本次改动（未提交）：名字的命名空间是 Bucket），跨桶删除只能用 file_id（并且要确认）。
    const CliRun one = fixture.business({"file", "get", "first.txt"});
    FMT_CHECK_EQ(one.exit_code, 0);
    const std::string first_id = grab_file_id(one);
    FMT_CHECK(!first_id.empty());

    // 交互式：答 n —— 必须「已取消」且**文件还在**
    FMT_CHECK_EQ(fixture.business({"bucket", "use", "beta"}).exit_code, 0);
    const CliRun declined = run_interactive_until_output(
        fixture, "file delete " + first_id + "\nn\nbucket use alpha\nfile list\nexit\n");
    if (!has(declined, "确认执行？")) {
        std::fprintf(stderr, "[e2e] 交互式输出（exit=%d）：\n%s\n", declined.exit_code,
                     declined.output.c_str());
    }
    FMT_CHECK(has(declined, "确认执行？"));
    FMT_CHECK(has(declined, "已取消"));
    FMT_CHECK(has(declined, "first.txt"));  // 文件仍在 alpha 的列表里

    // 交互式：答 y —— 这次真的删掉
    FMT_CHECK_EQ(fixture.business({"bucket", "use", "beta"}).exit_code, 0);
    const CliRun accepted = run_interactive_until_output(
        fixture, "file delete " + first_id + "\ny\ntrash list\nexit\n");
    FMT_CHECK(has(accepted, "确认执行？"));
    FMT_CHECK(!has(accepted, "已取消"));
    FMT_CHECK(has(accepted, "[文件]"));
    FMT_CHECK(has(accepted, "first.txt"));
}
