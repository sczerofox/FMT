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

// Windows 上粘贴路径常带两类**看不见**的污染，屏幕上却完全正常：
//   * Explorer 的「复制路径」会给路径套一对引号：`"C:\a\b.jpg"`
//   * 从聊天窗口、网页、终端复制会夹进 Unicode 格式字符（Cf）：
//     U+202A 左右方向嵌入、U+200E/200F 方向标记、U+FEFF 零宽不换行空格、
//     U+00A0 不换行空格……它们按字面拼进路径，exists() 就找不到文件
//
// clean_user_path(): 去掉上述污染（引号**成对**才去，避免改掉名字里真的带引号的情况）。
std::string clean_user_path(std::string_view text);

// 文本里出现的不可见字符，按出现顺序去重，形如 {"U+202A", "U+202C"}。
// 报错时点出码位，用户才知道「路径明明是对的」到底哪里不对。
std::vector<std::string> invisible_characters(std::string_view text);

// 按分隔符切分；保留空字段（调用方自己决定要不要丢弃）。
std::vector<std::string> split(std::string_view text, char delimiter);

// 人类可读的文件大小：0B / 512B / 6KB / 1.5MB / 2GB。
std::string format_size(std::uint64_t bytes);

// 把反斜杠换成斜杠，便于日志与 JSON 中统一显示。
std::string to_forward_slashes(std::string text);

// URL 百分号编解码。
// 路径参数（例如 /api/bucket/<名称>）里的中文会被浏览器编码成 %E5%B7%A5…，
// 服务端必须先解码再当业务参数用。
//   url_decode："%E5%B7%A5" -> "工"；非法转义原样保留；'+' 不当空格。
//   url_encode：只保留 unreserved 字符（字母数字与 - _ . ~），其余转 %XX 大写。
std::string url_decode(std::string_view text);
std::string url_encode(std::string_view text);

}  // namespace fmt
