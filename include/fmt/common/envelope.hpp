// 统一响应信封
//
// 命名管道与 HTTP **共用同一个信封**（docx/FMT 项目架构.md §4.2）：
//
//   {"ok":true,  "data":{...}}
//   {"ok":false, "error":{"code":"FMT-305","message":"..."}}
//
// 管道帧额外带一个 id；HTTP 不带。错误码以字符串形式跨进程传递，两边用同一个
// 还原函数（code_from_string），所以 FMT-NNN 不会在传输中丢失语义。
#pragma once

#include <nlohmann/json.hpp>

#include "fmt/common/error.hpp"

namespace fmt {

nlohmann::json envelope_ok(nlohmann::json data);

nlohmann::json envelope_error(const Error& error);

}  // namespace fmt
