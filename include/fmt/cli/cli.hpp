// CLI：横幅、提示符、命令分发、单实例、提权
//
// 界面约定（docx/FMT 开发文档.md §127）：
//   FMT 1.0.0
//   Service Running...
//   fmt> service stop
//   需要管理员权限
//   正在提权...
//   执行成功...
//   错误码：0
//
// 正常结果走 stdout，错误走 stderr；命令失败后继续循环，不退出。
// CLI 不建目录、不写 JSON、不写日志文件：它只是客户端。
#pragma once

#include <string>
#include <vector>

#include "fmt/common/error.hpp"
#include "fmt/ipc/pipe.hpp"

namespace fmt::cli {

struct Options {
    std::string self_path;  // 本进程 exe 的绝对路径（提权副本要用同一个文件）
    std::string data_root;  // 要声明的数据根 = 本 exe 所在目录
};

// 进程入口的 CLI 分支：带参数执行一次，无参数进入交互循环。
int run(const std::vector<std::string>& args, const Options& options);

// 提权副本分支：--elevated <operation> --result <file>
int run_elevated(const std::vector<std::string>& args);

// 当前进程是否已经提权（TokenElevation）。
bool is_elevated();

// 把一行命令切成参数；支持双引号包裹带空格的参数。
std::vector<std::string> split_command(const std::string& line);

// 提权执行一条 service 命令，结果经临时文件回传（见 §13.8.5）。
struct ElevatedOutcome {
    bool ok = true;
    ErrorCode code = ErrorCode::Ok;
    std::string message;
    int exit_code = 0;
};

Result<ElevatedOutcome> elevate_service_command(const std::string& operation,
                                                const std::string& self_path);

// 提权副本把结果写到哪里：%TEMP%\fmt-elev-<pid>.json
std::string result_file_for(unsigned long process_id);

}  // namespace fmt::cli
