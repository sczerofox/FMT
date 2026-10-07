// 字符串工具
//
// 项目里所有对外文本都是 UTF-8；Windows API 需要 UTF-16，因此转换集中在这里。
// argv、环境变量、注册表读出来的宽字符串都要先过 to_utf8()。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fmt {

// UTF-16 <-> UTF-8。转换失败返回空字符串。
std::string to_utf8(std::wstring_view text);
std::wstring to_wide(std::string_view text);

bool starts_with(std::string_view text, std::string_view prefix);
bool ends_with(std::string_view text, std::string_view suffix);

// ASCII 大小写不敏感比较（命令行开关、扩展名判断用）。
bool iequals(std::string_view left, std::string_view right);

// 只转换 ASCII 字母，不动 UTF-8 多字节序列。
std::string to_lower(std::string_view text);

// 去掉首尾的空白（空格、制表符、回车、换行）。
std::string trim(std::string_view text);

// 按分隔符切分；保留空字段（调用方自己决定要不要丢弃）。
std::vector<std::string> split(std::string_view text, char delimiter);

// 人类可读的文件大小：0B / 512B / 6KB / 1.5MB / 2GB。
std::string format_size(std::uint64_t bytes);

// 把反斜杠换成斜杠，便于日志与 JSON 中统一显示。
std::string to_forward_slashes(std::string text);

}  // namespace fmt
