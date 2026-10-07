// 底层数据访问
//
// Storage 只做「读文件、写文件、建目录、移动、删除、检查」，不做业务规则：
// 能不能删由 File / Trash Service 决定，Storage 只负责执行。
//
// 关键文件一律「写 .tmp -> 校验 -> 替换」，避免异常退出留下半个文件
// （docx/FMT 开发文档.md §11、§90）。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "fmt/common/error.hpp"

namespace fmt {

// ---- 文本 ----
Result<std::string> read_text_file(const std::filesystem::path& path);

// 原子写：写 <path>.tmp -> 读回校验 -> 替换正式文件。
Status write_text_file_atomic(const std::filesystem::path& path, std::string_view content);

// ---- JSON ----
// 文件不存在返回 FileNotFound；内容不是合法 JSON 返回 JsonParseError。
Result<nlohmann::json> read_json_file(const std::filesystem::path& path);

// 缩进 2 空格，末尾补换行；中文原样输出（不转义成 \uXXXX）。
Status write_json_file(const std::filesystem::path& path, const nlohmann::json& value);

// 集合数据：{"version":1,"<collection>":[...]}
nlohmann::json make_collection(int version, std::string_view collection);

// 版本策略：只接受明确支持的版本，未知版本直接拒绝，不降级、不猜测。
// 缺 version 字段或不是对象 -> JsonParseError；版本不符 -> JsonUnsupportedVersion。
Status check_version(const nlohmann::json& value, int supported);

// ---- 文件系统 ----
Status ensure_directory(const std::filesystem::path& path);
bool path_exists(const std::filesystem::path& path);
bool directory_exists(const std::filesystem::path& path);
bool file_exists(const std::filesystem::path& path);

}  // namespace fmt
