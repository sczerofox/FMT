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

}  // namespace fmt
