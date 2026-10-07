// 路径基础工具
//
// FMT_ROOT = 可执行文件所在目录，**不是**当前工作目录：双击启动、从别的
// 目录用命令行启动、被 SCM 启动，三种情况必须得到同一个根。
#pragma once

#include <filesystem>
#include <string>

namespace fmt {

// 当前进程可执行文件的完整路径（GetModuleFileNameW）。
std::filesystem::path executable_path();

// 可执行文件所在目录，即默认数据根。
std::filesystem::path executable_directory();

// %ProgramData%\FMT —— 服务自身状态（service.json）存放处，不属于业务数据。
std::filesystem::path service_state_directory();

// 路径转 UTF-8 文本，日志与 JSON 里显示用。
std::string path_to_utf8(const std::filesystem::path& path);

// UTF-8 文本转路径。
std::filesystem::path path_from_utf8(const std::string& text);

// 相对数据根、正斜杠的路径文本：写进 JSON 用（trash.json 的 original_path 等）。
// 传进来的是根之外的路径时退回完整路径，不抛异常。
std::string relative_path_text(const std::filesystem::path& root,
                               const std::filesystem::path& path);

}  // namespace fmt
