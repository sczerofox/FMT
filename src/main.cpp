// FMT 入口点
//
// 三种形态都从这一个二进制进来：
//   1. Service 形态 —— 被 SCM 启动。必须在 wmain 开头尽早调用
//      StartServiceCtrlDispatcherW（SCM 只等 30 秒）。
//   2. 提权副本   —— 由 CLI 用 runas 拉起，短命，只做 SCM 操作（--elevated）。
//   3. CLI 形态   —— 用户双击或命令行启动。
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "fmt/cli/cli.hpp"
#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/service/runtime.hpp"

int wmain(int argc, wchar_t** argv) {
    // 控制台统一 UTF-8：输出与输入都按 UTF-8 处理，中文才不会乱码。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // ---- 1. 服务形态优先 ----
    switch (fmt::service::run_service_host()) {
        case fmt::service::HostResult::BecameService:
            return 0;
        case fmt::service::HostResult::Failed: {
            const fmt::ErrorCode code = fmt::ErrorCode::ServiceOperationFailed;
            std::fprintf(stderr, "%s 无法接入服务控制管理器\n错误码：%d\n",
                         fmt::code_string(code).c_str(), fmt::exit_code(code));
            return fmt::exit_code(code);
        }
        case fmt::service::HostResult::NotService:
            break;
    }

    // ---- 2./3. CLI 形态（含提权副本分支）----
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.push_back(fmt::to_utf8(argv[i]));
    }

    fmt::cli::Options options;
    options.self_path = fmt::path_to_utf8(fmt::executable_path());
    options.data_root = fmt::path_to_utf8(fmt::executable_directory());

    return fmt::cli::run(args, options);
}
