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
// CLI 不改业务数据（它是管道客户端），但会把「自己做了什么、结果如何」追加到
// 与 Service 共用的 <数据根>/log/fmt.log。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"
#include "fmt/ipc/pipe.hpp"

namespace fmt::cli {

// 一条业务命令的**参数信封**：位置参数放 `args.argv`，开关（dry_run / force 等）
// 放**同级**字段。
//
// 为什么要有这个函数：开关放错层级会抛 nlohmann 的 type_error.305
// （"cannot use operator[] with a string argument with array"），未捕获就是
// `abort()` 弹窗——Debug 版里表现为「Debug Error! abort() has been called」。
// 预检与真实请求必须用**同一个**信封构造函数，两边才不会各写一套、各错一处。
//
//   positional: 位置参数数组（可以为空）
//   dry_run   : 只预检、不改数据
//   force     : 已确认执行（破坏性操作）
nlohmann::json argument_envelope(const nlohmann::json& positional, bool dry_run = false,
                                 bool force = false);

// ---- CLI 侧日志 ----
//
// CLI 与 Service **往同一个** <数据根>/log/fmt.log 追加。旧口径是「只有 Service
// 写日志文件」，结果是用户在 CLI 里敲的命令在日志里完全看不到（例如敲完
// service stop，服务随即停了，日志里一个字都没有）。
//
// 两个进程都以追加方式打开同一个文件，每行一次写入；控制台输出仍由 CLI 自己
// 打印，所以日志器的控制台开关全部关掉，避免同一句话打印两遍。
//
// CLI 只允许创建 <数据根>/log/ 这一个目录（日志不是业务数据）；
// repository / data / config 一概不碰。
Result<std::unique_ptr<Logger>> open_cli_logger(const std::string& data_root);

// 进程内的 CLI 日志器；传 nullptr 表示不写日志（测试用）。
void set_logger(Logger* logger);
Logger* logger();
void log(LogLevel level, std::string_view module, const std::string& message);
void log_info(std::string_view module, const std::string& message);
void log_warn(std::string_view module, const std::string& message);
void log_error(std::string_view module, const std::string& message);

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

// 用户可以直接键入的 service 子命令 —— **每条都要提权**。
// `status` 不在其中（查询不提权），`reinstall` 在（提交后从引导内部用法改成正式命令）。
// 暴露出来是为了能把它钉住：这个集合决定「敲了什么会被当成什么」。
bool is_user_service_command(const std::string& operation);

// 把一行命令切成参数；支持双引号包裹带空格的参数。
std::vector<std::string> split_command(const std::string& line);

// 版本一行（程序名 + 版本 + 构建日期）。**三处共用同一份文本**：
//   * 交互窗口开头的横幅
//   * 一次性 `fmt.exe --version` / `-v`
//   * 命令 `version`（窗口里敲，或 `fmt.exe version`）
// 分开写就会漂移，所以只留这一个来源。
std::string version_text();

// 「服务数据根被搬走了」的提示文本（切换发生时打到 stderr）。
// 之所以要有这么一句：数据根由 CLI 声明，任何位置的 fmt.exe 一连上就会把服务
// 的数据根换成它自己所在目录——桶、文件、回收站会整体变成另一个目录的内容，
// 而这件事原来只写进日志，用户在控制台上看不到。
std::string root_switch_notice(const std::string& previous, const std::string& current);

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
